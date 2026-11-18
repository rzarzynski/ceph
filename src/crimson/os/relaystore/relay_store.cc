// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "crimson/os/relaystore/relay_store.h"

#include <ranges>
#include <seastar/core/coroutine.hh>

namespace crimson::os {

seastar::future<uint32_t> _RelayStore::start()
{
  storage_shards = co_await decorated_store.start();
  max_attr_name_length =
    decorated_store.get_sharded_store(0).get_max_attr_name_length();
  co_await shards.start(std::ref(*this));
  co_return storage_shards;
}

seastar::future<> _RelayStore::stop()
{
  co_await shards.stop();
  co_await decorated_store.stop();
}

_RelayStore::mount_ertr::future<> _RelayStore::mount()
{
  return decorated_store.mount().safe_then([this] {
    return discover_collections();
  });
}

seastar::future<> _RelayStore::umount()
{
  colls_discovered = false;
  coll_locations.clear();
  return decorated_store.umount();
}

seastar::future<> _RelayStore::discover_collections()
{
  ceph_assert(seastar::this_shard_id() == primary_core);
  coll_locations.clear();
  const auto smp_count = seastar::this_smp_shard_count();
  std::vector<uint32_t> per_owner_idx(smp_count, 0);

  for (uint32_t g = 0; g < storage_shards; ++g) {
    const core_id_t owner_core = g % smp_count;
    const store_index_t local_index = g / smp_count;
    auto colls = co_await crimson::submit_to(
      owner_core, [&decorated_store = this->decorated_store, local_index] {
        return decorated_store.get_sharded_store(local_index).list_shard_collections();
      });
    for (const auto& cid : colls) {
      core_id_t exposed_core = owner_core;
      if (cid == coll_t::meta()) {
        exposed_core = 0;
      } else if (storage_shards < smp_count) {
        const auto num_mapped = (smp_count - 1 - owner_core) / storage_shards + 1;
        exposed_core = owner_core + (per_owner_idx[owner_core]++ % num_mapped) * storage_shards;
      }
      coll_locations.emplace(cid, coll_loc_t{owner_core, local_index, exposed_core});
    }
  }
  colls_discovered = true;
}

seastar::future<std::vector<FuturizedStore::coll_core_t>>
_RelayStore::list_collections()
{
  ceph_assert(seastar::this_shard_id() == primary_core);
  if (!colls_discovered) {
    co_await discover_collections();
  }
  std::vector<coll_core_t> ret;
  ret.reserve(coll_locations.size());
  for (const auto& [cid, loc] : coll_locations) {
    ret.emplace_back(cid, loc.exposed_core);
  }
  co_return ret;
}

seastar::future<CollectionRef>
_RelayStore::Shard::create_new_collection(const coll_t& cid)
{
  const auto calling_core = seastar::this_shard_id();
  const auto smp_count = seastar::this_smp_shard_count();
  core_id_t owner_core = 0;
  store_index_t local_index = 0;
  core_id_t exposed_core = calling_core;

  if (cid == coll_t::meta()) {
    owner_core = 0;
    local_index = 0;
    exposed_core = 0;
  } else if (parent.storage_shards <= smp_count) {
    owner_core = calling_core % parent.storage_shards;
    local_index = 0;
  } else {
    owner_core = calling_core;
    const auto n_local = parent.local_shard_count_on(calling_core);
    ceph_assert(n_local > 0);
    local_index = next_local_index;
    next_local_index = (next_local_index + 1) % n_local;
  }

  auto open_on_owner = [&decorated_store = parent.decorated_store,
                        cid, owner_core, local_index]() {
    auto& target_shard = decorated_store.get_sharded_store(local_index);
    return target_shard.create_new_collection(cid).then(
      [owner_core, local_index, &target_shard](CollectionRef inner) {
        return std::make_shared<RelayCollection::TargetState>(
          seastar::foreign_ptr<CollectionRef>(std::move(inner)),
          owner_core,
          local_index,
          target_shard);
      });
  };

  std::shared_ptr<RelayCollection::TargetState> state;
  if (owner_core == calling_core) {
    state = co_await open_on_owner();
  } else {
    state = co_await seastar::smp::submit_to(owner_core, std::move(open_on_owner));
  }

  if (parent.storage_shards > smp_count) {
    local_created_colls[cid] = state;
  }

  auto loc = coll_loc_t{owner_core, local_index, exposed_core};
  if (calling_core == parent.primary_core) {
    parent.coll_locations[cid] = loc;
  } else {
    co_await seastar::smp::submit_to(
      parent.primary_core, [&parent = this->parent, cid, loc] {
        parent.coll_locations[cid] = loc;
      });
  }

  co_return CollectionRef(new RelayCollection(cid, std::move(state)));
}

seastar::future<CollectionRef>
_RelayStore::Shard::open_collection(const coll_t& cid)
{
  const auto calling_core = seastar::this_shard_id();
  std::optional<coll_loc_t> loc;

  if (cid == coll_t::meta()) {
    loc = coll_loc_t{0, 0, 0};
  } else {
    auto lookup = [&parent = this->parent, cid]() -> seastar::future<std::optional<coll_loc_t>> {
      if (!parent.colls_discovered) {
        co_await parent.discover_collections();
      }
      if (auto it = parent.coll_locations.find(cid); it != parent.coll_locations.end()) {
        co_return it->second;
      }
      co_return std::nullopt;
    };
    if (calling_core == parent.primary_core) {
      loc = co_await lookup();
    } else {
      loc = co_await seastar::smp::submit_to(parent.primary_core, std::move(lookup));
    }
  }

  if (!loc) {
    co_return CollectionRef{};
  }

  auto open_on_owner = [&decorated_store = parent.decorated_store,
                        cid, loc = *loc]()
    -> seastar::future<std::shared_ptr<RelayCollection::TargetState>> {
    auto& target_shard = decorated_store.get_sharded_store(loc.local_index);
    auto inner = co_await target_shard.open_collection(cid);
    if (!inner) {
      co_return nullptr;
    }
    co_return std::make_shared<RelayCollection::TargetState>(
      seastar::foreign_ptr<CollectionRef>(std::move(inner)),
      loc.owner_core,
      loc.local_index,
      target_shard);
  };

  std::shared_ptr<RelayCollection::TargetState> state;
  if (loc->owner_core == calling_core) {
    state = co_await open_on_owner();
  } else {
    state = co_await seastar::smp::submit_to(loc->owner_core, std::move(open_on_owner));
  }

  if (!state) {
    co_return CollectionRef{};
  }
  co_return CollectionRef(new RelayCollection(cid, std::move(state)));
}

seastar::future<> _RelayStore::Shard::rebind_split_collections(
  std::shared_ptr<RelayCollection::TargetState> parent_state,
  std::vector<coll_t> split_dest_colls)
{
  ceph_assert(parent_state->owner_core == seastar::this_shard_id());
  for (const auto& dest_cid : split_dest_colls) {
    if (auto it = local_created_colls.find(dest_cid); it != local_created_colls.end()) {
      if (auto child_state = it->second.lock();
          child_state && child_state->local_index != parent_state->local_index) {
        auto new_inner = co_await parent_state->target_shard->create_new_collection(dest_cid);
        child_state->inner = seastar::foreign_ptr<CollectionRef>(std::move(new_inner));
        child_state->owner_core = parent_state->owner_core;
        child_state->local_index = parent_state->local_index;
        child_state->target_shard = parent_state->target_shard;
        auto loc = coll_loc_t{
          parent_state->owner_core,
          parent_state->local_index,
          parent_state->owner_core};
        if (seastar::this_shard_id() == parent.primary_core) {
          parent.coll_locations[dest_cid] = loc;
        } else {
          co_await seastar::smp::submit_to(
            parent.primary_core, [&parent = this->parent, dest_cid, loc] {
              parent.coll_locations[dest_cid] = loc;
            });
        }
      }
    }
  }
}

seastar::future<> _RelayStore::Shard::do_transaction_no_callbacks(
  CollectionRef ch,
  ceph::os::Transaction&& txn)
{
  auto* relay_coll = static_cast<RelayCollection*>(ch.get());
  auto state = relay_coll->get_state();

  std::vector<coll_t> removed_colls;
  std::vector<coll_t> split_dest_colls;
  const bool may_split = parent.storage_shards > seastar::this_smp_shard_count();

  for (auto i = txn.begin(); i.have_op(); ) {
    auto* op = i.decode_op();
    if (may_split && op->op == ceph::os::Transaction::OP_SPLIT_COLLECTION2) {
      split_dest_colls.push_back(i.get_cid(op->dest_cid));
    } else if (op->op == ceph::os::Transaction::OP_RMCOLL ||
               op->op == ceph::os::Transaction::OP_MERGE_COLLECTION) {
      removed_colls.push_back(i.get_cid(op->cid));
    }
  }

  auto dispatch = [this, state, removed_colls = std::move(removed_colls)](
                    ceph::os::Transaction&& t) mutable {
    seastar::future<> fut = seastar::now();
    if (state->owner_core == seastar::this_shard_id()) {
      fut = state->target_shard->do_transaction_no_callbacks(
        state->inner.get(), std::move(t));
    } else {
      fut = seastar::smp::submit_to(
        state->owner_core,
        [state, t = std::move(t)]() mutable {
          return state->target_shard->do_transaction_no_callbacks(
            state->inner.get(), std::move(t));
        });
    }
    if (removed_colls.empty()) {
      return fut;
    }
    return fut.then([this, removed_colls = std::move(removed_colls)]() mutable {
      for (const auto& cid : removed_colls) {
        local_created_colls.erase(cid);
      }
      if (seastar::this_shard_id() == parent.primary_core) {
        for (const auto& cid : removed_colls) {
          parent.coll_locations.erase(cid);
        }
        return seastar::now();
      }
      return seastar::smp::submit_to(
        parent.primary_core,
        [&parent = this->parent, removed_colls = std::move(removed_colls)] {
          for (const auto& cid : removed_colls) {
            parent.coll_locations.erase(cid);
          }
        });
    });
  };

  if (split_dest_colls.empty()) {
    return dispatch(std::move(txn));
  }

  return rebind_split_collections(state, std::move(split_dest_colls)).then(
    [dispatch = std::move(dispatch), txn = std::move(txn)]() mutable {
      return dispatch(std::move(txn));
    });
}

template <auto Method>
seastar::future<> _RelayStore::Shard::fan_out_inject(const ghobject_t& o)
{
  const auto calling_core = seastar::this_shard_id();
  const auto smp_count = seastar::this_smp_shard_count();
  if (parent.storage_shards <= smp_count) {
    const core_id_t owner_core = calling_core % parent.storage_shards;
    if (owner_core == calling_core) {
      return (parent.decorated_store.get_sharded_store(0).*Method)(o);
    }
    return crimson::submit_to(
      owner_core, [&decorated_store = parent.decorated_store, o] {
        return (decorated_store.get_sharded_store(0).*Method)(o);
      });
  }
  const auto n_local = parent.local_shard_count_on(calling_core);
  return seastar::parallel_for_each(
    std::views::iota(store_index_t{0}, n_local),
    [&decorated_store = parent.decorated_store, o](store_index_t idx) {
      return (decorated_store.get_sharded_store(idx).*Method)(o);
    });
}

seastar::future<> _RelayStore::Shard::inject_data_error(const ghobject_t& o)
{
  return fan_out_inject<&FuturizedStore::Shard::inject_data_error>(o);
}

seastar::future<> _RelayStore::Shard::inject_mdata_error(const ghobject_t& o)
{
  return fan_out_inject<&FuturizedStore::Shard::inject_mdata_error>(o);
}

} // namespace crimson::os
