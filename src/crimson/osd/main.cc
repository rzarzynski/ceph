// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#include <iostream>

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>

#include "common/ceph_argparse.h"
#include "crimson/common/config_proxy.h"
#include "crimson/mon/MonClient.h"
#include "crimson/net/SocketMessenger.h"

#include "osd.h"

using config_t = ceph::common::ConfigProxy;

void usage(const char* prog) {
  std::cout << "usage: " << prog << " -i <ID>" << std::endl;
  generic_server_usage();
}

int main(int argc, char* argv[])
{
  std::vector<const char*> args{argv + 1, argv + argc};
  if (ceph_argparse_need_usage(args)) {
    usage(argv[0]);
    return EXIT_SUCCESS;
  }
  std::string cluster;
  std::string conf_file_list;
  // ceph_argparse_early_args() could _exit(), while local_conf() won't ready
  // until it's started. so do the boilerplate-settings parsing here.
  auto init_params = ceph_argparse_early_args(args,
                                              CEPH_ENTITY_TYPE_OSD,
                                              &cluster,
                                              &conf_file_list);
  int return_value = 0;
  // TODO: add heartbeat
  seastar::app_template app;
  // talk with osd
  // ceph::net::SocketMessenger::msgrptr_t cluster_msgrsrv = nullptr;

  // talk with mon/mgr
  ceph::mon::Client::clntptr_t moncsrv;
  seastar::sharded<OSD> osd;

  args.insert(begin(args), argv[0]);
  try {
    return app.run(args.size(), const_cast<char**>(args.data()), [&] {
        return seastar::async([&moncsrv, &osd,
                               &init_params, &cluster, &conf_file_list,
                               &return_value] {
        ceph::common::sharded_conf().start(init_params.name, cluster).get();
        seastar::engine().at_exit([] {
          return ceph::common::sharded_conf().stop();
        });
        ceph::common::sharded_perf_coll().start().get();
        seastar::engine().at_exit([] {
          return ceph::common::sharded_perf_coll().stop();
        });
        auto& conf = ceph::common::local_conf();
        conf.parse_config_files(conf_file_list).get();
        const auto whoami = std::stoi(conf->name.get_id());

	ceph::net::SocketMessenger::create(entity_name_t::OSD(whoami),
          static_cast<const char*>("monc"), 0).then(
	  [] (auto monc_msgr) mutable {
            if (ceph::common::local_conf()->ms_crc_data) {
              monc_msgr->set_crc_data();
            }
            if (ceph::common::local_conf()->ms_crc_header) {
              monc_msgr->set_crc_header();
            }
	    return std::move(monc_msgr);
	  }).then([] (auto monc_msgr) mutable {
	    return ceph::mon::Client::create(std::move(monc_msgr));
	  }).then([ &moncsrv ] (auto monc) mutable {
	    moncsrv = std::move(monc);
	    return moncsrv->start();
	  });

	ceph::net::SocketMessenger::create(entity_name_t::OSD(whoami, "osdc", 0).then(
	  [] (auto msgr) {
            if (ceph::common::local_conf()->ms_crc_data) {
              msgr->set_crc_data();
            }
            if (ceph::common::local_conf()->ms_crc_header) {
              msgr->set_crc_header();
            }
	  });

        seastar::engine().at_exit([&client_msgr] {
          return client_msgr.shutdown();
        });

        monc.start().get();
        seastar::engine().at_exit([&monc] {
          return monc.stop();
        });

        osd.start().get();
        seastar::engine().at_exit([&osd, &return_value] {
          return osd.stop().then([&return_value] {
            ::_exit(return_value);
          });
        });

      }).then_wrapped([&return_value] (auto&& f) {
        try {
          f.get();
        } catch (...) {
          return_value = 1;
          seastar::engine_exit(std::current_exception());
        }
      });
    });
  } catch (...) {
    seastar::fprint(std::cerr, "FATAL: Exception during startup, aborting: %s\n", std::current_exception());
    return EXIT_FAILURE;
  }
}

/*
 * Local Variables:
 * compile-command: "make -j4 \
 * -C ../../../build \
 * crimson-osd"
 * End:
 */
