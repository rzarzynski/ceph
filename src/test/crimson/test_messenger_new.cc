#include "messages/MPing.h"
#include "crimson/common/log.h"
#include "crimson/net/Connection.h"
#include "crimson/net/Dispatcher.h"
#include "crimson/net/SocketMessenger.h"

#include <map>
#include <random>
#include <boost/program_options.hpp>
#include <seastar/core/app-template.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>

namespace bpo = boost::program_options;

namespace {

seastar::logger& logger() {
  return ceph::get_logger(ceph_subsys_ms);
}

template <typename T, typename... Args>
seastar::future<T*> create_sharded(Args... args) {
  auto sharded_obj = seastar::make_lw_shared<seastar::sharded<T>>();
  return sharded_obj->start(args...).then([sharded_obj]() {
      auto& ret = sharded_obj->local();
      seastar::engine().at_exit([sharded_obj]() {
          return sharded_obj->stop().finally([sharded_obj] {});
        });
      return &ret;
    });
}

std::random_device rd;
std::default_random_engine rng{rd()};
bool verbose = false;

seastar::future<> test_echo(unsigned rounds,
                            double keepalive_ratio)
{
  struct test_state {
    struct Server final
        : public ceph::net::Dispatcher,
          public seastar::peering_sharded_service<Server> {
      ceph::net::Messenger *msgr = nullptr;

      Dispatcher* get_local_shard() override {
        return &(container().local());
      }
      seastar::future<> stop() {
        return seastar::make_ready_future<>();
      }
      seastar::future<> ms_dispatch(ceph::net::ConnectionRef c,
                                    MessageRef m) override {
        if (verbose) {
          logger().info("server got {}", *m);
        }
        // reply with a pong
        return c->send(MessageRef{new MPing(), false});
      }

      seastar::future<> init(const entity_name_t& name,
                             const entity_addr_t& addr,
                             const std::string& lname,
                             const uint64_t nonce) {
        return ceph::net::SocketMessenger::create(name, lname, nonce).then(
          [ this, addr ](auto messenger) {
            auto local_msgr = messenger.get();
            return container().invoke_on_all([ local_msgr ](auto& server) {
                server.msgr = local_msgr->get_local_shard();
              }).then([ local_msgr, addr] {
                return local_msgr->bind(addr);
              }).then([this, local_msgr] {
                return local_msgr->start(this);
              });
          });
      }
      seastar::future<> shutdown() {
        ceph_assert(msgr);
        return msgr->shutdown();
      }
    };

    struct Client final
        : public ceph::net::Dispatcher,
          public seastar::peering_sharded_service<Client> {

      struct PingSession : public seastar::enable_shared_from_this<PingSession> {
        unsigned count = 0u;
      };

      unsigned rounds;
      std::bernoulli_distribution keepalive_dist;
      ceph::net::Messenger *msgr = nullptr;
      std::map<ceph::net::Connection*, seastar::promise<>> pending_conns;

      Client(unsigned rounds, double keepalive_ratio)
        : rounds(rounds),
          keepalive_dist(std::bernoulli_distribution{keepalive_ratio}) {}
      Dispatcher* get_local_shard() override {
        return &(container().local());
      }
      seastar::future<> stop() {
        return seastar::now();
      }
      seastar::future<> ms_handle_connect(ceph::net::ConnectionRef conn) override {
        logger().info("{}: connected to {}", *conn, conn->get_peer_addr());
        auto session = seastar::make_shared<PingSession>();
        conn->set_priv(session);
        return container().invoke_on_all([conn = conn.get()](auto& client) {
            auto [i, added] = client.pending_conns.emplace(conn, seastar::promise<>());
            std::ignore = i;
            ceph_assert(added);
          });
      }
      seastar::future<> ms_dispatch(ceph::net::ConnectionRef c,
                                    MessageRef m) override {
        // NOTE: we have to use reinterpret_cast because
        // enable_shared_from_this privately inherits shared_ptr_count_base.
        auto session = reinterpret_cast<PingSession*>(c->get_priv().get());
        ++(session->count);
        if (verbose) {
          logger().info("client ms_dispatch {}", session->count);
        }

        if (session->count == rounds) {
          logger().info("{}: finished receiving {} pongs", *c.get(), session->count);
          return container().invoke_on_all([conn = c.get()](auto &client) {
              auto found = client.pending_conns.find(conn);
              ceph_assert(found != client.pending_conns.end());
              found->second.set_value();
            });
        } else {
          return seastar::now();
        }
      }

      seastar::future<> init(const entity_name_t& name,
                             const std::string& lname,
                             const uint64_t nonce) {
        return create_sharded<ceph::net::SocketMessenger>(name, lname, nonce)
          .then([this](auto messenger) {
            return container().invoke_on_all([messenger](auto& client) {
                client.msgr = messenger->get_local_shard();
              }).then([this, messenger] {
                return messenger->start(this);
              });
          });
      }

      seastar::future<> shutdown() {
        ceph_assert(msgr);
        return msgr->shutdown();
      }

      seastar::future<> dispatch_pingpong(const entity_addr_t& peer_addr, bool foreign_dispatch=true) {
        return msgr->connect(peer_addr, entity_name_t::TYPE_OSD)
          .then([this, foreign_dispatch](auto conn) {
            if (foreign_dispatch) {
              return do_dispatch_pingpong(&**conn)
                .finally([this, conn] {});
            } else {
              // NOTE: this could be faster if we don't switch cores in do_dispatch_pingpong().
              return container().invoke_on(conn->get()->shard_id(), [conn = &**conn](auto &client) {
                  return client.do_dispatch_pingpong(conn);
                }).finally([this, conn] {});
            }
          });
      }

     private:
      seastar::future<> do_dispatch_pingpong(ceph::net::Connection* conn) {
        return seastar::do_with(unsigned(0), unsigned(0),
                                [this, conn](auto &count_ping, auto &count_keepalive) {
            return seastar::do_until(
              [this, conn, &count_ping, &count_keepalive] {
                bool stop = (count_ping == rounds);
                if (stop) {
                  logger().info("{}: finished sending {} pings with {} keepalives",
                                *conn, count_ping, count_keepalive);
                }
                return stop;
              },
              [this, conn, &count_ping, &count_keepalive] {
                return seastar::repeat([this, conn, &count_ping, &count_keepalive] {
                    if (keepalive_dist(rng)) {
                      return conn->keepalive()
                        .then([&count_keepalive] {
                          count_keepalive += 1;
                          return seastar::make_ready_future<seastar::stop_iteration>(
                            seastar::stop_iteration::no);
                        });
                    } else {
                      count_ping += 1;
                      return conn->send(MessageRef{new MPing(), false})
                        .then([] {
                          return seastar::make_ready_future<seastar::stop_iteration>(
                            seastar::stop_iteration::yes);
                        });
                    }
                  });
              }).then([this, conn] {
                auto found = pending_conns.find(conn);
                if (found == pending_conns.end())
                  throw std::runtime_error{"Not connected."};
                return found->second.get_future();
              });
          });
      }
    };
  };

  typedef std::tuple<seastar::future<test_state::Server*>,
                     seastar::future<test_state::Server*>,
                     seastar::future<test_state::Client*>,
                     seastar::future<test_state::Client*>> result_tuple;
  return seastar::when_all(
      create_sharded<test_state::Server>(),
      create_sharded<test_state::Server>(),
      create_sharded<test_state::Client>(rounds, keepalive_ratio),
      create_sharded<test_state::Client>(rounds, keepalive_ratio))
    .then([rounds, keepalive_ratio](result_tuple t) {
      auto server1 = std::get<0>(t).get0();
      auto server2 = std::get<1>(t).get0();
      auto client1 = std::get<2>(t).get0();
      auto client2 = std::get<3>(t).get0();
      // start servers and clients
      entity_addr_t addr1;
      addr1.set_type(entity_addr_t::TYPE_LEGACY);
      addr1.set_family(AF_INET);
      addr1.set_port(9010);
      entity_addr_t addr2;
      addr2.set_type(entity_addr_t::TYPE_LEGACY);
      addr2.set_family(AF_INET);
      addr2.set_port(9011);
      return seastar::when_all_succeed(
          server1->init(entity_name_t::OSD(0), addr1, "server1", 1),
          server2->init(entity_name_t::OSD(1), addr2, "server2", 2),
          client1->init(entity_name_t::OSD(2), "client1", 3),
          client2->init(entity_name_t::OSD(3), "client2", 4))
      // dispatch pingpoing
        .then([client1, client2] {
          entity_addr_t peer_addr1;
          peer_addr1.set_type(entity_addr_t::TYPE_LEGACY);
          peer_addr1.parse("127.0.0.1:9010/1", nullptr);
          entity_addr_t peer_addr2;
          peer_addr2.set_type(entity_addr_t::TYPE_LEGACY);
          peer_addr2.parse("127.0.0.1:9011/2", nullptr);
          return seastar::when_all_succeed(
              client1->dispatch_pingpong(peer_addr1, true),
              client1->dispatch_pingpong(peer_addr2, false),
              client2->dispatch_pingpong(peer_addr1, false),
              client2->dispatch_pingpong(peer_addr2, true));
      // shutdown
        }).finally([client1] {
          logger().info("client1 shutdown...");
          return client1->shutdown();
        }).finally([client2] {
          logger().info("client2 shutdown...");
          return client2->shutdown();
        }).finally([server1] {
          logger().info("server1 shutdown...");
          return server1->shutdown();
        }).finally([server2] {
          logger().info("server2 shutdown...");
          return server2->shutdown();
        });
    });
}

}

int main(int argc, char** argv)
{
  seastar::app_template app;
  app.add_options()
    ("verbose,v", bpo::value<bool>()->default_value(false),
     "chatty if true")
    ("rounds", bpo::value<unsigned>()->default_value(512),
     "number of pingpong rounds")
    ("keepalive-ratio", bpo::value<double>()->default_value(0.1),
     "ratio of keepalive in ping messages");
  return app.run(argc, argv, [&app] {
      auto&& config = app.configuration();
      verbose = config["verbose"].as<bool>();
      auto rounds = config["rounds"].as<unsigned>();
      auto keepalive_ratio = config["keepalive-ratio"].as<double>();
      return test_echo(rounds, keepalive_ratio)
        .then([] {
        //  return test_concurrent_dispatch();
        //}).then([] {
          std::cout << "All tests succeeded" << std::endl;
        }).handle_exception([] (auto eptr) {
          std::cout << "Test failure" << std::endl;
          return seastar::make_exception_future<>(eptr);
        });
    });
}
