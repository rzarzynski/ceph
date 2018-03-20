// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
//
// Ceph - scalable distributed file system
//
// Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
//
// This is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License version 2.1, as published by the Free Software
// Foundation.  See file COPYING.
//

#ifndef CEPH_COMMON_TINY_VECTOR_H
#define CEPH_COMMON_TINY_VECTOR_H

#include <type_traits>

#include "include/intarith.h"

namespace ceph::containers {

// tiny_vector - a CPU-friendly container like small_vector but for
// mutexes, atomics and other non-movable things.
//
// The purpose of the container is store arbitrary number of objects
// with absolutely minimal requirements regarding constructibility
// and assignability while minimizing memory indirection.
// There is no obligation for MoveConstructibility, CopyConstructibility,
// MoveAssignability, CopyAssignability nor even DefaultConstructibility
// which allows to handle std::mutexes, std::atomics or any type embedding
// them.
//
// Few requirements translate into tiny interface. The container isn't
// Copy- nor MoveConstructible. Although it does offer random access
// iterator, insertion in the middle is not allowed. The maximum number
// of elements must be known at run-time. This shouldn't be an issue in
// the intended use case: sharding.
//
// Alternatives:
//  1. std::vector<boost::optional<ValueT>> initialized with the known
//     size and emplace_backed(). boost::optional inside provides
//     the DefaultConstructibility. Imposes extra memory indirection.
//  2. boost::container::small_vector + boost::optional always
//     requires MoveConstructibility.
//  3. boost::container::static_vector feed via emplace_back().
//     Good for performance but enforces upper limit on elements count.
//     For sharding this means we can't handle arbitrary number of
//     shards (weird configs).
//  4. std::unique_ptr<ValueT>: extra indirection together with memory
//     fragmentation.

template<typename Value,
         std::size_t Capacity,
         std::size_t Alignment = alignof(Value)>
class tiny_vector {
  using storage_unit_t = \
    std::aligned_storage_t<sizeof(Value), Alignment>;

  std::size_t _size = 0;
  storage_unit_t* const data = nullptr;
  storage_unit_t internal[Capacity];

public:
  typedef std::size_t size_type;
  typedef std::add_lvalue_reference_t<Value> reference;
  typedef std::add_const_t<reference> const_reference;
  typedef std::add_pointer_t<Value> pointer;

  class emplacer {
    friend class tiny_vector;

    tiny_vector* parent;
    emplacer(tiny_vector* const parent)
      : parent(parent) {
    }

  public:
    template<class... Args>
    void emplace(Args&&... args) {
      if (!parent) {
        return;
      }
      new (&parent->data[parent->_size++]) Value(std::forward<Args>(args)...);
      parent = nullptr;
    }
  };

  template<typename F>
  tiny_vector(const std::size_t count, F&& f)
    : data(count <= Capacity ? internal
                             : new storage_unit_t[count]) {
    for (std::size_t i = 0; i < count; ++i) {
      // caller MAY emplace up to `count` elements but it IS NOT
      // obliged to do so. The emplacer guarantees that the limit
      // will never be exceeded.
      f(i, emplacer(this));
    }
  }

  ~tiny_vector() {
    for (auto& elem : *this) {
      elem.~Value();
    }

    const auto data_addr = reinterpret_cast<uintptr_t>(data);
    const auto this_addr = reinterpret_cast<uintptr_t>(this);
    if (data_addr < this_addr ||
        data_addr >= this_addr + sizeof(*this)) {
      delete[] data;
    }
  }

  reference       operator[](size_type pos) {
    return reinterpret_cast<reference>(data[pos]);
  }
  const_reference operator[](size_type pos) const {
    return reinterpret_cast<const_reference>(data[pos]);
  }

  size_type size() const {
    return _size;
  }

  pointer begin() {
    return reinterpret_cast<pointer>(&data[0]);
  }
  pointer end() {
    return reinterpret_cast<pointer>(&data[_size]);
  }

  const pointer begin() const {
    return reinterpret_cast<pointer>(&data[0]);
  }
  const pointer end() const {
    return reinterpret_cast<pointer>(&data[_size]);
  }

  const pointer cbegin() const {
    return reinterpret_cast<pointer>(&data[0]);
  }
  const pointer cend() const {
    return reinterpret_cast<pointer>(&data[_size]);
  }
};


static constexpr const std::size_t CACHE_LINE_SIZE = 64;

template<typename Value, std::size_t Capacity>
class shard_vector : public tiny_vector<Value, Capacity, CACHE_LINE_SIZE> {
  typedef tiny_vector<Value, Capacity, CACHE_LINE_SIZE> base_t;
  typedef typename base_t::size_type size_type;
  typedef typename base_t::reference reference;
  typedef typename base_t::const_reference const_reference;

public:
  template<typename F>
  shard_vector(const ceph::math::p2_t<size_type> count, F&& f)
    : base_t(count, std::forward<F>(f)) {
    assert(count == base_t::size());
  }

  ceph::math::p2_t<size_type> size() const {
    return ceph::math::p2_t<size_type>::from_p2(base_t::size());
  }

  reference get_shard(const size_type selector) {
    return (*this)[selector % size()];
  }

  const_reference get_shard(const size_type selector) const {
    return (*this)[selector % size()];
  }
};

} // namespace ceph::containers

#endif // CEPH_COMMON_TINY_VECTOR_H
