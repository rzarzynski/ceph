// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>

#include "crimson/common/smp_helpers.h"
#include "crimson/os/futurized_collection.h"
#include "crimson/os/futurized_store.h"

namespace crimson::os {

class _RelayStore;

class RelayCollection final : public FuturizedCollection {
public:
  struct TargetState {
    seastar::foreign_ptr<CollectionRef> inner;
    core_id_t owner_core = 0;
    store_index_t local_index = 0;
    FuturizedStore::Shard* target_shard = nullptr;

    TargetState(
      seastar::foreign_ptr<CollectionRef> inner,
      core_id_t owner_core,
      store_index_t local_index,
      FuturizedStore::Shard& target_shard)
      : inner(std::move(inner)),
        owner_core(owner_core),
        local_index(local_index),
        target_shard(&target_shard) {}
  };

  RelayCollection(
    const coll_t& cid,
    std::shared_ptr<TargetState> state)
    : FuturizedCollection(cid),
      state(std::move(state)) {}

  const std::shared_ptr<TargetState>& get_state() const {
    return state;
  }

  seastar::future<> flush() override {
    auto s = state;
    if (s->owner_core == seastar::this_shard_id()) {
      return s->inner->flush();
    }
    const auto owner_core = s->owner_core;
    return crimson::submit_to(owner_core, [s = std::move(s)] {
      return s->inner->flush();
    });
  }

  seastar::future<bool> flush_commit() override {
    auto s = state;
    if (s->owner_core == seastar::this_shard_id()) {
      return s->inner->flush_commit();
    }
    const auto owner_core = s->owner_core;
    return crimson::submit_to(owner_core, [s = std::move(s)] {
      return s->inner->flush_commit();
    });
  }

private:
  std::shared_ptr<TargetState> state;
};

class _RelayStore final : public FuturizedStore {
public:
  struct coll_loc_t {
    core_id_t owner_core = 0;
    store_index_t local_index = 0;
    core_id_t exposed_core = 0;
  };

  class Shard final : public FuturizedStore::Shard {
    using base_t = FuturizedStore::Shard;
    _RelayStore& parent;
    store_index_t next_local_index = 0;
    // Tracks live collections created on this reactor when
    // storage_shards > smp_count so OP_SPLIT_COLLECTION2 can rebind
    // child collections to the parent's storage shard.
    std::unordered_map<coll_t, std::weak_ptr<RelayCollection::TargetState>>
      local_created_colls;

    template <auto MemberFunc, typename... Args>
    static auto dispatch_coll_op(CollectionRef ch, Args&&... args)
    {
      auto* relay_coll = static_cast<RelayCollection*>(ch.get());
      const auto& state = relay_coll->get_state();
      if (state->owner_core == seastar::this_shard_id()) {
        return (state->target_shard->*MemberFunc)(
          state->inner.get(), std::forward<Args>(args)...);
      } else {
        auto state_copy = state;
        const auto owner_core = state_copy->owner_core;
        return crimson::submit_to(
          owner_core,
          [state_copy = std::move(state_copy),
           args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(
              [&state_copy](auto&... unpacked_args) {
                return (state_copy->target_shard->*MemberFunc)(
                  state_copy->inner.get(),
                  unpacked_args...);
              },
              args_tuple);
          });
      }
    }

  public:
    explicit Shard(_RelayStore& parent)
      : parent(parent) {}
    ~Shard() override = default;

    seastar::future<> stop() {
      local_created_colls.clear();
      return seastar::now();
    }

    seastar::future<bool> is_coll_storage_full(CollectionRef ch) const final {
      auto* relay_coll = static_cast<RelayCollection*>(ch.get());
      const auto& state = relay_coll->get_state();
      if (state->owner_core == seastar::this_shard_id()) {
        return seastar::make_ready_future<bool>(state->target_shard->is_storage_full());
      }
      auto state_copy = state;
      const auto owner_core = state_copy->owner_core;
      return crimson::submit_to(
        owner_core,
        [state_copy = std::move(state_copy)] {
          return state_copy->target_shard->is_storage_full();
        });
    }

    stat_ertr::future<struct stat> stat(
      CollectionRef c,
      const ghobject_t& oid,
      uint32_t op_flags = 0) final
    {
      return dispatch_coll_op<&base_t::stat>(std::move(c), oid, op_flags);
    }

    read_errorator::future<ceph::bufferlist> read(
      CollectionRef c,
      const ghobject_t& oid,
      uint64_t offset,
      size_t len,
      uint32_t op_flags = 0) final
    {
      return dispatch_coll_op<&base_t::read>(std::move(c), oid, offset, len, op_flags);
    }

    read_errorator::future<ceph::bufferlist> readv(
      CollectionRef c,
      const ghobject_t& oid,
      interval_set<uint64_t>& m,
      uint32_t op_flags = 0) final
    {
      return dispatch_coll_op<&base_t::readv>(std::move(c), oid, m, op_flags);
    }

    base_errorator::future<bool> exists(
      CollectionRef ch,
      const ghobject_t& oid,
      uint32_t op_flags = 0) final
    {
      return dispatch_coll_op<&base_t::exists>(std::move(ch), oid, op_flags);
    }

    get_attr_errorator::future<ceph::bufferlist> get_attr(
      CollectionRef c,
      const ghobject_t& oid,
      std::string_view name,
      uint32_t op_flags = 0) const final
    {
      auto* relay_coll = static_cast<RelayCollection*>(c.get());
      const auto& state = relay_coll->get_state();
      if (state->owner_core == seastar::this_shard_id()) {
        return state->target_shard->get_attr(state->inner.get(), oid, name, op_flags);
      }
      auto state_copy = state;
      const auto owner_core = state_copy->owner_core;
      return crimson::submit_to(
        owner_core,
        [state_copy = std::move(state_copy),
         oid, key = std::string(name), op_flags] {
          return state_copy->target_shard->get_attr(
            state_copy->inner.get(), oid, key, op_flags);
        });
    }

    get_attrs_ertr::future<attrs_t> get_attrs(
      CollectionRef c,
      const ghobject_t& oid,
      uint32_t op_flags = 0) final
    {
      return dispatch_coll_op<&base_t::get_attrs>(std::move(c), oid, op_flags);
    }

    read_errorator::future<omap_values_t> omap_get_values(
      CollectionRef c,
      const ghobject_t& oid,
      const omap_keys_t& keys,
      uint32_t op_flags = 0) final
    {
      return dispatch_coll_op<&base_t::omap_get_values>(std::move(c), oid, keys, op_flags);
    }

    read_errorator::future<ObjectStore::omap_iter_ret_t> omap_iterate(
      CollectionRef c,
      const ghobject_t& oid,
      ObjectStore::omap_iter_seek_t start_from,
      omap_iterate_cb_t callback,
      uint32_t op_flags = 0,
      omap_iterate_conf_t on_conflict = nullptr) final
    {
      return dispatch_coll_op<&base_t::omap_iterate>(
        std::move(c), oid, start_from, callback, op_flags, on_conflict);
    }

    get_attr_errorator::future<bufferlist> omap_get_header(
      CollectionRef c,
      const ghobject_t& oid,
      uint32_t op_flags = 0) final
    {
      return dispatch_coll_op<&base_t::omap_get_header>(std::move(c), oid, op_flags);
    }

    seastar::future<std::tuple<std::vector<ghobject_t>, ghobject_t>> list_objects(
      CollectionRef c,
      const ghobject_t& start,
      const ghobject_t& end,
      uint64_t limit,
      uint32_t op_flags = 0) const final
    {
      return dispatch_coll_op<&base_t::list_objects>(
        std::move(c), start, end, limit, op_flags);
    }

    seastar::future<CollectionRef> create_new_collection(const coll_t& cid) final;

    seastar::future<CollectionRef> open_collection(const coll_t& cid) final;

    seastar::future<> set_collection_opts(
      CollectionRef c,
      const pool_opts_t& opts) final
    {
      return dispatch_coll_op<&base_t::set_collection_opts>(std::move(c), opts);
    }

    seastar::future<> do_transaction_no_callbacks(
      CollectionRef ch,
      ceph::os::Transaction&& txn) final;

    seastar::future<> flush(CollectionRef ch) final {
      return dispatch_coll_op<&base_t::flush>(std::move(ch));
    }

    seastar::future<> inject_data_error(const ghobject_t& o) final;
    seastar::future<> inject_mdata_error(const ghobject_t& o) final;

    read_errorator::future<fiemap_ret_t> fiemap(
      CollectionRef ch,
      const ghobject_t& oid,
      uint64_t off,
      uint64_t len,
      uint32_t op_flags = 0) final
    {
      return dispatch_coll_op<&base_t::fiemap>(std::move(ch), oid, off, len, op_flags);
    }

    unsigned get_max_attr_name_length() const final {
      return parent.max_attr_name_length;
    }

  private:
    seastar::future<> rebind_split_collections(
      std::shared_ptr<RelayCollection::TargetState> parent_state,
      std::vector<coll_t> split_dest_colls);

    template <auto Method>
    seastar::future<> fan_out_inject(const ghobject_t& o);
  };

  explicit _RelayStore(std::unique_ptr<FuturizedStore> store)
    : _decorated_store(std::move(store)),
      decorated_store(*_decorated_store) {}

  explicit _RelayStore(FuturizedStore& store)
    : decorated_store(store) {}

  ~_RelayStore() override = default;

  seastar::future<uint32_t> start() final;
  seastar::future<> stop() final;
  mount_ertr::future<> mount() final;
  seastar::future<> umount() final;

  mkfs_ertr::future<> mkfs(uuid_d new_osd_fsid) final {
    return decorated_store.mkfs(new_osd_fsid);
  }

  seastar::future<store_statfs_t> stat() const final {
    return decorated_store.stat();
  }

  seastar::future<store_statfs_t> pool_statfs(int64_t pool_id) const final {
    return decorated_store.pool_statfs(pool_id);
  }

  seastar::future<> report_stats() final {
    return decorated_store.report_stats();
  }

  uuid_d get_fsid() const final {
    return decorated_store.get_fsid();
  }

  uint64_t get_max_object_size() const final {
    return decorated_store.get_max_object_size();
  }

  seastar::future<> write_meta(
    const std::string& key,
    const std::string& value) final
  {
    return decorated_store.write_meta(key, value);
  }

  FuturizedStore::Shard& get_sharded_store(store_index_t store_index = 0) final {
    ceph_assert(store_index == 0);
    return shards.local();
  }

  seastar::future<std::tuple<int, std::string>> read_meta(
    const std::string& key) final
  {
    return decorated_store.read_meta(key);
  }

  seastar::future<std::vector<coll_core_t>> list_collections() final;

  seastar::future<std::string> get_default_device_class() final {
    return decorated_store.get_default_device_class();
  }

  seastar::future<std::string> get_data_backend_type_name() final {
    return decorated_store.get_data_backend_type_name();
  }

  seastar::future<std::string> get_cache_backend_type_name() final {
    return decorated_store.get_cache_backend_type_name();
  }

  seastar::future<> do_gc() final {
    return decorated_store.do_gc();
  }

  uint32_t get_storage_shard_count() const final {
    return storage_shards;
  }

private:
  seastar::future<> discover_collections();

  store_index_t local_shard_count_on(core_id_t core) const {
    const auto smp_count = seastar::this_smp_shard_count();
    if (core >= storage_shards) {
      return 0;
    }
    return (storage_shards - 1 - core) / smp_count + 1;
  }

  std::unique_ptr<FuturizedStore> _decorated_store;
  FuturizedStore& decorated_store;
  uint32_t storage_shards = 0;
  unsigned max_attr_name_length = 0;
  std::unordered_map<coll_t, coll_loc_t> coll_locations;
  bool colls_discovered = false;
  seastar::sharded<Shard> shards;
};

} // namespace crimson::os
