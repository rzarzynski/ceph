#include "ec_backend.h"

#include "messages/MOSDPGPull.h"

#include "crimson/osd/shard_services.h"

ECBackend::ECBackend(shard_id_t shard,
                     ECBackend::CollectionRef coll,
                     crimson::osd::ShardServices& shard_services,
		     crimson::osd::PG& pg,
                     const ec_profile_t&,
                     uint64_t)
  : PGBackend{shard, coll, &shard_services.get_store(), pg}
{
  // todo
}

ECBackend::ll_read_errorator::future<ceph::bufferlist>
ECBackend::_read(const hobject_t& hoid,
                 const uint64_t off,
                 const uint64_t len,
                 const uint32_t flags)
{
  // todo
  return seastar::make_ready_future<bufferlist>();
}

seastar::future<crimson::osd::acked_peers_t>
ECBackend::_submit_transaction(std::set<pg_shard_t>&& pg_shards,
                               const hobject_t& hoid,
                               ceph::os::Transaction&& txn,
                               const osd_op_params_t& osd_op_p,
                               epoch_t min_epoch, epoch_t max_epoch,
			       std::vector<pg_log_entry_t>&& log_entries)
{
  // todo
  return seastar::make_ready_future<crimson::osd::acked_peers_t>();
}

seastar::future<> ECBackend::recover_object(
  const hobject_t& soid,
  eversion_t need)
{
  return seastar::make_ready_future<>();
}

seastar::future<> ECBackend::recover_delete(
  const hobject_t& soid,
  eversion_t need)
{
  return seastar::make_ready_future<>();
}

seastar::future<> ECBackend::push_delete(
  const hobject_t& soid,
  eversion_t need)
{
  return seastar::make_ready_future<>();
}
seastar::future<> ECBackend::handle_pull(
  Ref<MOSDPGPull> m)
{
  return seastar::make_ready_future<>();
}
seastar::future<> ECBackend::handle_pull_response(
  Ref<MOSDPGPush> m)
{
  return seastar::make_ready_future<>();
}
seastar::future<> ECBackend::handle_push(
  Ref<MOSDPGPush> m)
{
  return seastar::make_ready_future<>();
}
seastar::future<> ECBackend::handle_push_reply(
  Ref<MOSDPGPushReply> m)
{
  return seastar::make_ready_future<>();
}
seastar::future<> ECBackend::handle_recovery_delete(
  Ref<MOSDPGRecoveryDelete> m)
{
  return seastar::make_ready_future<>();
}
