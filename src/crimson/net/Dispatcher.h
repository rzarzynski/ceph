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

#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>

#include "Fwd.h"

class AuthAuthorizer;

namespace ceph::net {

class Dispatcher {
 public:
  virtual ~Dispatcher() {}

  virtual seastar::future<> ms_dispatch(ConnectionRef conn, MessageRef m) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<> ms_handle_accept(ConnectionRef conn) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<> ms_handle_connect(ConnectionRef conn) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<> ms_handle_reset(ConnectionRef conn) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<> ms_handle_remote_reset(ConnectionRef conn) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<msgr_tag_t, bufferlist>
  ms_verify_authorizer(peer_type_t,
		       auth_proto_t,
		       bufferlist&) {
    return seastar::make_ready_future<msgr_tag_t, bufferlist>(0, bufferlist{});
  }
  virtual seastar::future<std::unique_ptr<AuthAuthorizer>>
  ms_get_authorizer(peer_type_t, bool force_new);

  // get the locol dispatcher shard if it is accessed by another core
  virtual Dispatcher* get_local_shard() {
    return this;
  }
};


// ForeignDispatcher -- a \c Dispatcher implementation that relays calls
// to a component residing on single, preselected engine. This component
// provides Dispatcher-like interface. The difference is about making
// inter-engine traffic more safe.

// The intended use scenario is connecting components requiring sharding
// with the solitary ones.
//
// \param DecorateeT the type of foreign quasi-Dispatcher implementation.
//	  CRTP allows to avoid the overhead of dynamic polymorphism.
template <class DecorateeT>
class ForeignDispatcher : public Dispatcher {
  const seastar::shard_id sid;

  DecorateeT& get_decoratee() {
    return static_cast<DecorateeT&>(*this);
  }

 public:
  ForeignDispatcher(const seastar::shard_id sid)
    : sid(sid) {
  }

  seastar::future<> ms_dispatch(ConnectionRef conn, MessageRef msg) final {
    // NOTE: this still might be troublesome when modifications are performed
    // remotely. To be honest, I don't know a language measure to mitigate
    // the problem entirely. Even passing e.g. pointer-to-const would be not
    // enough due to e.g. a `mutable` member.
    return seastar::smp::submit_to(sid,
      [ this,
	fconn = seastar::make_foreign(conn),
	fmsg = seastar::make_foreign(msg)
      ]() mutable {
	// we're using non-virtual call here. For more details please refer to
	// https://en.cppreference.com/w/cpp/language/virtual
	return get_decoratee().DecorateeT::fms_dispatch(std::move(fconn),
							std::move(fmsg));
      });
  }

  seastar::future<> ms_handle_accept(ConnectionRef conn) final {
    return seastar::smp::submit_to(sid,
      [ this, fconn = seastar::make_foreign(conn) ]() mutable {
	return get_decoratee().DecorateeT::fms_handle_accept(std::move(fconn));
      });
  }

  seastar::future<> ms_handle_connect(ConnectionRef conn) final {
    return seastar::smp::submit_to(sid,
      [ this, fconn = seastar::make_foreign(conn) ]() mutable {
	return get_decoratee().DecorateeT::fms_handle_connect(std::move(fconn));
      });
  }

  seastar::future<> ms_handle_reset(ConnectionRef conn) final {
    return seastar::smp::submit_to(sid,
      [ this, fconn = seastar::make_foreign(conn) ]() mutable {
	return get_decoratee().DecorateeT::fms_handle_reset(std::move(fconn));
      });
  }

  seastar::future<> ms_handle_remote_reset(ConnectionRef conn) final {
    return seastar::smp::submit_to(sid,
      [ this, fconn = seastar::make_foreign(conn) ]() mutable {
	return get_decoratee().DecorateeT::fms_handle_remote_reset(std::move(fconn));
      });
  }

  seastar::future<msgr_tag_t, bufferlist>
  ms_verify_authorizer(peer_type_t peer,
		       auth_proto_t proto,
		       ceph::bufferlist& bl) final {
    return seastar::smp::submit_to(sid,
      [ this, &bl, peer = std::move(peer), proto = std::move(proto) ] {
	return get_decoratee().DecorateeT::fms_verify_authorizer(
	  std::move(peer), std::move(proto), bl);
      });
  }

  seastar::future<std::unique_ptr<AuthAuthorizer>>
  ms_get_authorizer(peer_type_t peer, bool force_new) final {
    return seastar::smp::submit_to(sid,
      [ this, peer = std::move(peer), force_new ] {
	return get_decoratee().DecorateeT::fms_get_authorizer(
	  std::move(peer), force_new);
      });
  }

  Dispatcher* get_local_shard() final {
    return this;
  }

protected:
  using ConnectionFRef = seastar::foreign_ptr<ConnectionRef>;
  using MessageFRef = seastar::foreign_ptr<MessageRef>;

  // XXX: making the fms_ stuff `virtual` ONLY to let derivatees to use
  // `override`. Dynamic dispatch WILL NOT be used!
  // This only to provide empty default implementations just following
  // the Dispatcher's convention about defaults.
  virtual seastar::future<> fms_dispatch(ConnectionFRef, MessageFRef) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<> fms_handle_accept(ConnectionFRef) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<> fms_handle_connect(ConnectionFRef) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<> fms_handle_reset(ConnectionFRef) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<> fms_handle_remote_reset(ConnectionFRef) {
    return seastar::make_ready_future<>();
  }

  virtual seastar::future<msgr_tag_t, bufferlist>
  fms_verify_authorizer(peer_type_t,
		       auth_proto_t,
		       bufferlist&) {
    return seastar::make_ready_future<msgr_tag_t, bufferlist>(0, bufferlist{});
  }

  virtual seastar::future<std::unique_ptr<AuthAuthorizer>>
  fms_get_authorizer(peer_type_t, bool force_new) {
    return seastar::make_ready_future<std::unique_ptr<AuthAuthorizer>>(nullptr);
  }
};

} // namespace ceph::net
