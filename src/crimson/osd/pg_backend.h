// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <map>
#include <memory>
#include <string>
#include <boost/smart_ptr/local_shared_ptr.hpp>

#include "crimson/common/shared_lru.h"
#include "os/Transaction.h"
#include "crimson/os/cyan_store.h"
#include "osd/osd_types.h"
#include "osd/osd_internal_types.h"

struct hobject_t;
namespace ceph::os {
  class Collection;
  class CyanStore;
}

class PGBackend
{
protected:
  using CollectionRef = boost::intrusive_ptr<ceph::os::Collection>;
  using ec_profile_t = std::map<std::string, std::string>;

public:
  virtual ~PGBackend() = default;
  static std::unique_ptr<PGBackend> load(
    const spg_t pgid,
    const pg_pool_t& pool,
    seastar::lw_shared_ptr<ceph::os::CyanStore> store,
    const ec_profile_t& ec_profile);
  static std::unique_ptr<PGBackend> create(
    const spg_t pgid,
    const pg_pool_t& pool,
    seastar::lw_shared_ptr<ceph::os::CyanStore> store,
    const ec_profile_t& ec_profile);
  using cached_os_t = boost::local_shared_ptr<ObjectState>;
  seastar::future<cached_os_t> get_object_state(const hobject_t& oid);
  seastar::future<> evict_object_state(const hobject_t& oid);
  seastar::future<bufferlist> read(const object_info_t& oi,
				   uint64_t off,
				   uint64_t len,
				   size_t truncate_size,
				   uint32_t truncate_seq,
				   uint32_t flags);
  seastar::future<> writefull(
    ObjectState& os,
    const OSDOp& osd_op,
    ceph::os::Transaction& trans);
  seastar::future<> submit_transaction(ceph::os::Transaction&& txn);
  seastar::future<> mutate_object(
    cached_os_t&& os,
    ceph::os::Transaction&& txn,
    const MOSDOp& m);

protected:
  // instantiate via the ::load() or ::create() factories
  PGBackend(shard_id_t shard,
            CollectionRef coll,
            seastar::lw_shared_ptr<ceph::os::CyanStore> store);

  const shard_id_t shard;
  CollectionRef coll;
  seastar::lw_shared_ptr<ceph::os::CyanStore> store;

private:
  using cached_ss_t = boost::local_shared_ptr<SnapSet>;
  SharedLRU<hobject_t, SnapSet> ss_cache;
  seastar::future<cached_ss_t> _load_ss(const hobject_t& oid);
  SharedLRU<hobject_t, ObjectState> os_cache;
  seastar::future<cached_os_t> _load_os(const hobject_t& oid);
  virtual seastar::future<bufferlist> _read(const hobject_t& hoid,
					    size_t offset,
					    size_t length,
					    uint32_t flags) = 0;
  static std::unique_ptr<PGBackend> _create_backend(
    CollectionRef coll,
    const shard_id_t& shard,
    const pg_pool_t& pool,
    seastar::lw_shared_ptr<ceph::os::CyanStore> store,
    const ec_profile_t& ec_profile);
  bool maybe_create_new_object(ObjectState& os, ceph::os::Transaction& txn);
};
