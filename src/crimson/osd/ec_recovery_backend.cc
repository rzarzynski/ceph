// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab expandtab

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <seastar/core/future.hh>
#include <seastar/core/do_with.hh>

#include "crimson/osd/ec_backend.h"
#include "crimson/osd/ec_recovery_backend.h"
#include "crimson/osd/pg.h"
#include "crimson/osd/pg_backend.h"
#include "messages/MOSDPGPush.h"
#include "messages/MOSDPGPushReply.h"
#include "msg/Message.h"
#include "osd/osd_types_fmt.h"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_osd);
  }
}

ECRecoveryBackend::ECRecoveryBackend(
  crimson::osd::PG& pg,
  crimson::osd::ShardServices& shard_services,
  crimson::os::CollectionRef coll,
  ECBackend* backend)
: ::RecoveryBackend(pg, shard_services, coll, backend),
  ::ECCommon::RecoveryBackend(
    shard_services.get_cct(),
    coll->get_cid(),
    backend->ec_impl,
    backend->sinfo,
    backend->read_pipeline,
    backend->unstable_hashinfo_registry,
    &pg)
{}

RecoveryBackend::interruptible_future<>
ECRecoveryBackend::recover_object(
  const hobject_t& soid,
  eversion_t need)
{
  logger().debug("{}: {}, {}", __func__, soid, need);
  return seastar::now();
}

void ECRecoveryBackend::commit_txn_send_replies(
  ceph::os::Transaction&& txn,
  std::map<int, MOSDPGPushReply*> replies)
{
}

RecoveryBackend::interruptible_future<>
ECRecoveryBackend::handle_push(
  Ref<MOSDPGPush> m)
{
  logger().debug("{}: {}", __func__, *m);
  RecoveryMessages rm;
  for (const auto& push_op : m->pushes) {
    handle_recovery_push(push_op, &rm, m->is_repair);
  }
  dispatch_recovery_messages(rm, m->get_priority());
  return seastar::now();
}

RecoveryBackend::interruptible_future<>
ECRecoveryBackend::handle_push_reply(
  Ref<MOSDPGPushReply> m)
{
  logger().debug("{}: {}", __func__, *m);
  RecoveryMessages rm;
  for (const auto& push_reply_op : m->replies) {
    handle_recovery_push_reply(push_reply_op, m->from, &rm);
  }
  dispatch_recovery_messages(rm, m->get_priority());
  return seastar::now();
}

RecoveryBackend::interruptible_future<>
ECRecoveryBackend::handle_recovery_op(
  Ref<MOSDFastDispatchOp> m,
  crimson::net::ConnectionXcoreRef conn)
{
  switch (m->get_header().type) {
  case MSG_OSD_PG_PUSH:
    return handle_push(boost::static_pointer_cast<MOSDPGPush>(m));
  case MSG_OSD_PG_PUSH_REPLY:
    return handle_push_reply(
      boost::static_pointer_cast<MOSDPGPushReply>(m));
  default:
    // delegate to parent class for handling backend-agnostic recovery ops.
    return ::RecoveryBackend::handle_recovery_op(std::move(m), conn);
  }
}
