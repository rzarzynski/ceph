// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <seastar/core/future.hh>
#include "include/buffer_fwd.h"
#include "osd/osd_types.h"
#include "pg_backend.h"

class MOSDPGPull;

class ECBackend : public PGBackend
{
public:
  ECBackend(shard_id_t shard,
	    CollectionRef coll,
	    crimson::osd::ShardServices& shard_services,
	    crimson::osd::PG& pg,
	    const ec_profile_t& ec_profile,
	    uint64_t stripe_width);

  // recovery related
  seastar::future<> recover_object(
    const hobject_t& soid,
    eversion_t need) final;
  seastar::future<> recover_delete(
    const hobject_t& soid,
    eversion_t need) final;
  seastar::future<> push_delete(
    const hobject_t& soid,
    eversion_t need) final;
  seastar::future<> handle_pull(
    Ref<MOSDPGPull> m) final;
  seastar::future<> handle_pull_response(
    Ref<MOSDPGPush> m) final;
  seastar::future<> handle_push(
    Ref<MOSDPGPush> m) final;
  seastar::future<> handle_push_reply(
    Ref<MOSDPGPushReply> m) final;
  seastar::future<> handle_recovery_delete(
    Ref<MOSDPGRecoveryDelete> m) final;

private:
  ll_read_errorator::future<ceph::bufferlist> _read(const hobject_t& hoid,
                                                    uint64_t off,
                                                    uint64_t len,
                                                    uint32_t flags) override;
  seastar::future<crimson::osd::acked_peers_t>
  _submit_transaction(std::set<pg_shard_t>&& pg_shards,
		      const hobject_t& hoid,
		      ceph::os::Transaction&& txn,
		      const osd_op_params_t& req,
		      epoch_t min_epoch, epoch_t max_epoch,
		      std::vector<pg_log_entry_t>&& log_entries) final;
  CollectionRef coll;
  crimson::os::FuturizedStore* store;
};
