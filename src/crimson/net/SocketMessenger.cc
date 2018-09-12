// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 Red Hat, Inc
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "SocketMessenger.h"

#include <tuple>
#include <boost/functional/hash.hpp>

#include "auth/Auth.h"
#include "Errors.h"
#include "Dispatcher.h"

using namespace ceph::net;

SocketMessenger::SocketMessenger(const entity_name_t& myname,
                                 const std::string& logic_name,
                                 uint32_t nonce)
  : Messenger{myname},
    sid{seastar::engine().cpu_id()},
    logic_name{logic_name},
    nonce{nonce}
{
  entity_addr_t my_addr;
  my_addr.set_type(entity_addr_t::TYPE_DEFAULT);
  my_addr.nonce = nonce;
  Messenger::set_myaddr(my_addr);
}

seastar::future<> SocketMessenger::set_myaddr(const entity_addr_t& addr)
{
  entity_addr_t my_addr = addr;
  my_addr.nonce = nonce;
  return container().invoke_on_all([my_addr](auto& msgr) {
      return msgr.Messenger::set_myaddr(my_addr);
    });
}

seastar::future<> SocketMessenger::bind(const entity_addr_t& addr)
{
  return container().invoke_on_all([addr](auto& msgr) {
      msgr.do_bind(addr);
    });
}

seastar::future<> SocketMessenger::start(Dispatcher *disp) {
  return container().invoke_on_all([disp](auto& msgr) {
      return msgr.do_start(disp->get_local_shard());
    });
}

seastar::future<ceph::net::ConnectionXRef>
SocketMessenger::connect(const entity_addr_t& peer_addr, const entity_type_t& peer_type)
{
  auto shard = locate_shard(peer_addr);
  return container().invoke_on(shard, [peer_addr, peer_type](auto& msgr) {
      return msgr.do_connect(peer_addr, peer_type);
    }).then([](seastar::foreign_ptr<ConnectionRef>&& conn) {
      return seastar::make_lw_shared<seastar::foreign_ptr<ConnectionRef>>(std::move(conn));
    });
}

seastar::future<> SocketMessenger::shutdown()
{
  return container().invoke_on_all([](auto& msgr) {
      return msgr.do_shutdown();
    }).finally([this] {
      return container().invoke_on_all([](auto& msgr) {
          msgr.shutdown_promise.set_value();
        });
    });
}

void SocketMessenger::do_bind(const entity_addr_t& addr)
{
  ceph_assert(addr.get_family() == AF_INET);

  entity_addr_t my_addr = addr;
  my_addr.nonce = nonce;
  Messenger::set_myaddr(my_addr);

  seastar::socket_address address(addr.in4_addr());
  seastar::listen_options lo;
  lo.reuse_address = true;
  listener = seastar::listen(address, lo);
}

seastar::future<> SocketMessenger::do_start(Dispatcher *disp)
{
  dispatcher = disp;

  // start listening if bind() was called
  if (listener) {
    seastar::keep_doing([this] {
        return listener->accept()
          .then([this] (seastar::connected_socket socket,
                        seastar::socket_address paddr) {
            // allocate the connection
            entity_addr_t peer_addr;
            peer_addr.set_sockaddr(&paddr.as_posix_sockaddr());
            auto shard = locate_shard(peer_addr);
            // don't wait before accepting another
            container().invoke_on(shard, [socket = std::move(socket), peer_addr, this](auto& msgr) mutable {
                SocketConnectionRef conn = seastar::make_shared<SocketConnection>(msgr, *msgr.dispatcher);
                conn->start_accept(std::move(socket), peer_addr);
              });
          });
      }).handle_exception_type([this] (const std::system_error& e) {
        // stop gracefully on connection_aborted
        if (e.code() != error::connection_aborted) {
          throw e;
        }
      });
  }

  return seastar::now();
}

seastar::foreign_ptr<ceph::net::ConnectionRef>
SocketMessenger::do_connect(const entity_addr_t& peer_addr, const entity_type_t& peer_type)
{
  if (auto found = lookup_conn(peer_addr); found) {
    return seastar::make_foreign(found->shared_from_this());
  }
  SocketConnectionRef conn = seastar::make_shared<SocketConnection>(*this, *dispatcher);
  conn->start_connect(peer_addr, peer_type);
  return seastar::make_foreign(conn->shared_from_this());
}

seastar::future<> SocketMessenger::do_shutdown()
{
  if (listener) {
    listener->abort_accept();
  }
  // close all connections
  return seastar::parallel_for_each(accepting_conns, [] (auto conn) {
      return conn->close();
    }).then([this] {
      ceph_assert(accepting_conns.empty());
      return seastar::parallel_for_each(connections, [] (auto conn) {
          return conn.second->close();
        });
    }).finally([this] {
      ceph_assert(connections.empty());
    });
}

seastar::future<> SocketMessenger::learned_addr(const entity_addr_t &peer_addr_for_me)
{
  if (!get_myaddr().is_blank_ip()) {
    // already learned or binded
    return seastar::now();
  }

  // Only learn IP address if blank.
  entity_addr_t addr = get_myaddr();
  addr.u = peer_addr_for_me.u;
  addr.set_port(get_myaddr().get_port());
  return set_myaddr(addr);
}

void SocketMessenger::set_default_policy(const SocketPolicy& p)
{
  policy_set.set_default(p);
}

void SocketMessenger::set_policy(entity_type_t peer_type,
				 const SocketPolicy& p)
{
  policy_set.set(peer_type, p);
}

void SocketMessenger::set_policy_throttler(entity_type_t peer_type,
					   Throttle* throttle)
{
  // only byte throttler is used in OSD
  policy_set.set_throttlers(peer_type, throttle, nullptr);
}

seastar::shard_id SocketMessenger::locate_shard(const entity_addr_t& addr)
{
  ceph_assert(addr.get_family() == AF_INET);
  std::size_t seed = 0;
  boost::hash_combine(seed, addr.u.sin.sin_addr.s_addr);
  //boost::hash_combine(seed, addr.u.sin.sin_port);
  //boost::hash_combine(seed, addr.nonce);
  return seed % seastar::smp::count;
}

ceph::net::SocketConnectionRef SocketMessenger::lookup_conn(const entity_addr_t& addr)
{
  if (auto found = connections.find(addr);
      found != connections.end()) {
    return found->second;
  } else {
    return nullptr;
  }
}

void SocketMessenger::accept_conn(SocketConnectionRef conn)
{
  accepting_conns.insert(conn);
}

void SocketMessenger::unaccept_conn(SocketConnectionRef conn)
{
  accepting_conns.erase(conn);
}

void SocketMessenger::register_conn(SocketConnectionRef conn)
{
  auto [i, added] = connections.emplace(conn->get_peer_addr(), conn);
  std::ignore = i;
  ceph_assert(added);
}

void SocketMessenger::unregister_conn(SocketConnectionRef conn)
{
  ceph_assert(conn);
  auto found = connections.find(conn->get_peer_addr());
  ceph_assert(found != connections.end());
  ceph_assert(found->second == conn);
  connections.erase(found);
}
