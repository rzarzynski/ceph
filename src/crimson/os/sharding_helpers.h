// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>

#include "crimson/common/smp_helpers.h"
#include "include/ceph_assert.h"

namespace crimson::os {

struct shard_desc_t {
  size_t local_index; // local in the domain of same reactor
  size_t global_shards_num; // number of storage shards across all reactors

  size_t global_index() const {
    auto gidx = seastar::this_shard_id() + seastar::this_smp_shard_count() * local_index;
    ceph_assert(gidx < global_shards_num);
    return gidx;
  }
};

template <class ServiceT>
class simplysharded : private seastar::sharded<ServiceT> {
  using base_t = seastar::sharded<ServiceT>;
public:
  template <typename... Args>
  seastar::future<> start(size_t num_shards, Args&&... args) noexcept;

  size_t local_count() const noexcept {
    return 1;
  }

  template <typename Func, typename... Args>
  seastar::future<> invoke_on_locals(Func func, Args... args) noexcept {
    return seastar::futurize_invoke(func, this->local(), args...);
  }

  using base_t::stop;
  using base_t::local;
  using base_t::map_reduce0;
  using base_t::map;
  using base_t::invoke_on_all;
};

template <typename ServiceT>
template <typename... Args>
seastar::future<>
simplysharded<ServiceT>::start(const size_t num_shards, Args&&... args) noexcept
{
  // never ever change num of shards after mkfs
  ceph_assert(num_shards == seastar::this_smp_shard_count());
  return base_t::start(
    shard_desc_t{0, num_shards},
    std::forward<Args>(args)...);
}

template <class ServiceT>
class multisharded {
  std::vector<std::unique_ptr<ServiceT>> _instances;
public:
  template <typename... Args>
  seastar::future<> start(size_t num_shards, Args&&... args) noexcept;
  seastar::future<> stop() noexcept;

  size_t local_count() const noexcept {
    const auto smp_count = seastar::this_smp_shard_count();
    const auto tid = seastar::this_shard_id();
    if (tid >= _instances.size()) {
      return 0;
    }
    return (_instances.size() - 1 - tid) / smp_count + 1;
  }

  ServiceT& local(const size_t local_store_index = 0) noexcept;
  const ServiceT& local(const size_t local_store_index = 0) const noexcept;

  template <typename Func, typename... Args>
  seastar::future<> invoke_on_all(Func func, Args... args) noexcept;

  template <typename Func, typename... Args>
  seastar::future<> invoke_on_locals(Func func, Args... args) noexcept;

  template <typename Mapper, typename Initial, typename Reduce>
  seastar::future<Initial>
  map_reduce0(Mapper map, Initial initial, Reduce reduce) const;
};

template <typename ServiceT>
template <typename... Args>
seastar::future<>
multisharded<ServiceT>::start(const std::size_t num_shards, Args&&... args) noexcept
{
  _instances.resize(num_shards);
  auto args_tuple = std::make_tuple(std::forward<Args>(args)...);
  return seastar::parallel_for_each(
    std::views::iota(size_t{0}, num_shards),
    [this, num_shards, args_tuple = std::move(args_tuple)](size_t c) {
      return seastar::smp::submit_to(
        c % seastar::this_smp_shard_count(),
        [this, c, num_shards, args_tuple]() mutable {
          this->_instances[c] = std::apply(
            [c, num_shards](auto&... unpacked_args) {
              return std::make_unique<ServiceT>(
                shard_desc_t{c / seastar::this_smp_shard_count(), num_shards},
                unpacked_args...);
            },
            args_tuple);
          return seastar::now();
        });
    });
}

template <typename ServiceT>
seastar::future<>
multisharded<ServiceT>::stop() noexcept
{
  return seastar::parallel_for_each(
    std::views::iota(size_t{0}, _instances.size()),
    [this](size_t c) {
      return seastar::smp::submit_to(
        c % seastar::this_smp_shard_count(),
        [this, c] {
          this->_instances[c].reset();
          return seastar::now();
        });
    }).then([this] {
      this->_instances.clear();
    });
}

template <typename ServiceT>
ServiceT& multisharded<ServiceT>::local(const size_t local_store_index) noexcept
{
  const size_t global_idx =
    (local_store_index * seastar::this_smp_shard_count()) + seastar::this_shard_id();
  ceph_assert(global_idx < _instances.size());
  ceph_assert(_instances[global_idx] != nullptr);
  return *_instances[global_idx];
}

template <typename ServiceT>
const ServiceT& multisharded<ServiceT>::local(const size_t local_store_index) const noexcept
{
  const size_t global_idx =
    (local_store_index * seastar::this_smp_shard_count()) + seastar::this_shard_id();
  ceph_assert(global_idx < _instances.size());
  ceph_assert(_instances[global_idx] != nullptr);
  return *_instances[global_idx];
}

template <typename ServiceT>
template <typename Func, typename... Args>
seastar::future<>
multisharded<ServiceT>::invoke_on_all(Func func, Args... args) noexcept
{
  return seastar::parallel_for_each(
    std::views::iota(size_t{0}, _instances.size()),
    [this, func, args...](size_t c) mutable {
      return seastar::smp::submit_to(
        c % seastar::this_smp_shard_count(),
        [this, c, func, args...]() mutable {
          return seastar::futurize_invoke(func, *(this->_instances[c]), args...);
        });
    });
}

template <typename ServiceT>
template <typename Func, typename... Args>
seastar::future<>
multisharded<ServiceT>::invoke_on_locals(Func func, Args... args) noexcept
{
  const auto n_local = local_count();
  const auto smp_count = seastar::this_smp_shard_count();
  const auto tid = seastar::this_shard_id();
  return seastar::do_for_each(
    std::views::iota(size_t{0}, n_local),
    [this, tid, smp_count, func, args...](size_t local_idx) mutable {
      const size_t global_idx = tid + local_idx * smp_count;
      return seastar::futurize_invoke(func, *(this->_instances[global_idx]), args...);
    });
}

template <typename ServiceT>
template <typename Mapper, typename Initial, typename Reduce>
seastar::future<Initial>
multisharded<ServiceT>::map_reduce0(Mapper map, Initial initial, Reduce reduce) const
{
  auto wrapped_map = [this, map = std::move(map)](size_t c) {
    return seastar::smp::submit_to(
      c % seastar::this_smp_shard_count(),
      [this, map, c]() mutable {
        return seastar::futurize_invoke(map, *this->_instances[c]);
      });
  };
  return ::seastar::map_reduce(
    std::views::iota(size_t{0}, _instances.size()),
    std::move(wrapped_map),
    std::move(initial),
    std::move(reduce));
}

} // namespace crimson::os
