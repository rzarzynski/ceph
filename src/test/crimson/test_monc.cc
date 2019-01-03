#include <seastar/core/app-template.hh>
#include "common/ceph_argparse.h"
#include "crimson/common/config_proxy.h"
#include "crimson/mon/MonClient.h"
#include "crimson/net/Connection.h"
#include "crimson/net/SocketMessenger.h"

using Config = ceph::common::ConfigProxy;
using MonClient = ceph::mon::Client;

namespace {
  seastar::logger& logger()
  {
    return ceph::get_logger(ceph_subsys_monc);
  }
}

static seastar::future<> test_monc()
{
  return ceph::common::sharded_conf().start(EntityName{}, string_view{"ceph"}).then([] {
    std::vector<const char*> args;
    std::string cluster;
    std::string conf_file_list;
    auto init_params = ceph_argparse_early_args(args,
                                                CEPH_ENTITY_TYPE_CLIENT,
                                                &cluster,
                                                &conf_file_list);
    auto& conf = ceph::common::local_conf();
    conf->name = init_params.name;
    conf->cluster = cluster;
    return conf.parse_config_files(conf_file_list);
  }).then([] {
    ceph::common::sharded_perf_coll().start().then([] {
      seastar::engine().at_exit([] {
        return ceph::common::sharded_perf_coll().stop();
      });
    });
  }).then([] {
    auto&& msgr_fut = \
      ceph::net::SocketMessenger::create(entity_name_t::OSD(0), (const char*)"monc", 0);
    return msgr_fut.then([](auto local_msgr) {
      auto& conf = ceph::common::local_conf();
      if (conf->ms_crc_data) {
        local_msgr->set_crc_data();
      }
      if (conf->ms_crc_header) {
        local_msgr->set_crc_header();
      }
      return seastar::do_with(MonClient{*local_msgr}, std::move(local_msgr),
                              [](auto& monc, auto& local_msgr) {
        return local_msgr->start(&monc).then([&monc] {
          return seastar::with_timeout(
            seastar::lowres_clock::now() + std::chrono::seconds{5},
            monc.start());
        }).finally([&monc] {
          logger().info("{}:{} finally - maybe timeout", __func__, __LINE__);
          return monc.stop();
        }).finally([ &local_msgr ] {
          return local_msgr->shutdown();
        });
      });
    });
  }).finally([] {
    return ceph::common::sharded_perf_coll().stop().then([] {
      return ceph::common::sharded_conf().stop();
    });
  });
}

int main(int argc, char** argv)
{
  seastar::app_template app;
  return app.run(argc, argv, [&] {

    return test_monc().then([] {
      std::cout << "All tests succeeded" << std::endl;
    }).handle_exception([] (auto eptr) {
      std::cout << "Test failure" << std::endl;
      return seastar::make_exception_future<>(eptr);
    });
  });
}


/*
 * Local Variables:
 * compile-command: "make -j4 \
 * -C ../../../build \
 * unittest_seastar_monc"
 * End:
 */
