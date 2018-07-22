// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#pragma once

#include <ostream>
#include <variant>

#include "include/types.h"
#include "include/utime.h"
#include "osd/OpRequest.h"
#include "osd/PG.h"
#include "PGPeeringEvent.h"

class OSD;
class OSDShard;

namespace OpInterface {
  class OrderLocker {
  public:
    using Ref = unique_ptr<OrderLocker>;
    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual ~OrderLocker() {}
  };

  class OpQueueable {
  public:
    enum class op_type_t {
      client_op,
      peering_event,
      bg_snaptrim,
      bg_recovery,
      bg_scrub,
      bg_pg_delete
    };
    using Ref = std::unique_ptr<OpQueueable>;

    /// Items with the same queue token will end up in the same shard
    virtual uint32_t get_queue_token() const = 0;

    /* Items will be dequeued and locked atomically w.r.t. other items with the
       * same ordering token */
    virtual const spg_t& get_ordering_token() const = 0;
    virtual OrderLocker::Ref get_order_locker(PGRef pg) = 0;
    virtual op_type_t get_op_type() const = 0;
    virtual boost::optional<OpRequestRef> maybe_get_op() const {
      return boost::none;
    }

    virtual uint64_t get_reserved_pushes() const {
      return 0;
    }

    virtual bool is_peering() const {
      return false;
    }
    virtual bool peering_requires_pg() const {
      ceph_abort();
    }
    virtual const PGCreateInfo *creates_pg() const {
      return nullptr;
    }

    virtual ostream &print(ostream &rhs) const = 0;

    virtual void run(OSD *osd, OSDShard *sdata, PGRef& pg, ThreadPool::TPHandle &handle) = 0;
    virtual ~OpQueueable() {}
    friend ostream& operator<<(ostream& out, const OpQueueable& q) {
      return q.print(out);
    }

  };
}

/// Implements boilerplate for operations queued for the pg lock
class PGOpQueueable : public OpInterface::OpQueueable {
  spg_t pgid;
protected:
  const spg_t& get_pgid() const {
    return pgid;
  }
public:
  explicit PGOpQueueable(spg_t pg) : pgid(pg) {}
  uint32_t get_queue_token() const override final {
    return get_pgid().ps();
  }

  const spg_t& get_ordering_token() const override final {
    return get_pgid();
  }

  OpInterface::OrderLocker::Ref get_order_locker(PGRef pg) override final {
    class Locker : public OpInterface::OrderLocker {
      PGRef pg;
    public:
      explicit Locker(PGRef pg) : pg(pg) {}
      void lock() override final {
	pg->lock();
      }
      void unlock() override final {
	pg->unlock();
      }
    };
    return OpInterface::OrderLocker::Ref(
      new Locker(pg));
  }
};

class PGOpItem : public PGOpQueueable {
  OpRequestRef op;
public:
  PGOpItem(spg_t pg, OpRequestRef op) : PGOpQueueable(pg), op(op) {}
  op_type_t get_op_type() const override final {
    return op_type_t::client_op;
  }
  ostream &print(ostream &rhs) const override final {
    return rhs << "PGOpItem(op=" << *(op->get_req()) << ")";
  }
  boost::optional<OpRequestRef> maybe_get_op() const override final {
    return op;
  }
  void run(OSD *osd, OSDShard *sdata, PGRef& pg, ThreadPool::TPHandle &handle) override final;
};

template <class InterfaceT,
	  class SpecializationT>
struct inline_ptr {
  enum class op_t {
    copy, move, destroy, size
  };

  // Nice implementation of C++ type erasure. Taken from Adam Emerson.
  template <class T>
  static std::size_t op_fun(op_t oper, void* p1, void* p2) {
    auto me = static_cast<T*>(p1);

    switch (oper) {
    case op_t::copy:
      // One conspicuous downside is that immovable/uncopyable functions
      // kill compilation right here, even if nobody ever calls the move
      // or copy methods. Working around this is a pain, since we'd need
      // four operator functions and a top-level class to
      // provide/withhold copy/move operations as appropriate.
//      new (p2) T(*me);
      ceph_abort();
      break;

    case op_t::move:
      new (p2) T(std::move(*me));
      break;

    case op_t::destroy:
      me->~T();
      break;

    case op_t::size:
      return sizeof(T);
    }
    return 0;
  }

  std::size_t (*operate)(op_t, void*, void*) = nullptr;

  typedef std::unique_ptr<InterfaceT> unique_ptr_type;

  typedef InterfaceT interface_type;
  typedef SpecializationT specialization_type;

  mutable typename std::aligned_storage<
    std::max(sizeof(InterfaceT), sizeof(SpecializationT))>::type storage;

  inline_ptr(unique_ptr_type&& p)
    : operate(op_fun<unique_ptr_type>) {
    new (&storage) unique_ptr_type(std::move(p));
  }

  template <class... ArgsT>
  inline_ptr(std::in_place_type_t<SpecializationT>, ArgsT&&... args)
    : operate(op_fun<SpecializationT>) {
    new (&storage) SpecializationT(std::forward<ArgsT>(args)...);
  }

  ~inline_ptr() {
    operate(op_t::destroy, &storage, nullptr);
  }
};

// Using a macro because I cannot find a counterpart of
//    pointer_to_derived->Derived::virtual_method() nor
//    reference_to_derived.Derived::virtual_method()
// for pointer-to-members. There are some other possibilities. TODO.
#define inline_ptr_call(obj, method_name, ...) \
do { \
    if (likely(obj.operate == &decltype(obj)::op_fun<decltype(obj)::specialization_type>)) { \
      auto& i = reinterpret_cast<decltype(obj)::specialization_type&>(obj.storage); \
      return i.decltype(obj)::specialization_type::method_name(__VA_ARGS__); \
    } else { \
      auto& i = reinterpret_cast<decltype(obj)::unique_ptr_type&>(obj.storage); \
      return i->method_name(__VA_ARGS__); \
    } \
} while (0) 

class OpQueueItem {
public:
  // Abstraction for operations queueable in the op queue
  using OrderLocker = OpInterface::OrderLocker;
  using OpQueueable = OpInterface::OpQueueable;

private:
  inline_ptr<OpQueueable, PGOpItem> qitem_alt;
  int cost;
  unsigned priority;
  utime_t start_time;
  uint64_t owner;  ///< global id (e.g., client.XXX)
  epoch_t map_epoch;    ///< an epoch we expect the PG to exist in

public:
  OpQueueItem(
    OpQueueable::Ref &&item,
    int cost,
    unsigned priority,
    utime_t start_time,
    uint64_t owner,
    epoch_t e)
    : qitem_alt(std::move(item)),
      cost(cost),
      priority(priority),
      start_time(start_time),
      owner(owner),
      map_epoch(e)
  {}
  template <class... ArgsT>
  OpQueueItem(
    int cost,
    unsigned priority,
    utime_t start_time,
    uint64_t owner,
    epoch_t e,
    std::in_place_type_t<PGOpItem>,
    ArgsT... args)
    : qitem_alt(std::in_place_type_t<PGOpItem>{}, std::forward<ArgsT>(args)...),
      cost(cost),
      priority(priority),
      start_time(start_time),
      owner(owner),
      map_epoch(e)
  {}
  OpQueueItem(OpQueueItem &&) = default;
  OpQueueItem(const OpQueueItem &) = delete;
  OpQueueItem &operator=(OpQueueItem &&) = default;
  OpQueueItem &operator=(const OpQueueItem &) = delete;

  OrderLocker::Ref get_order_locker(PGRef pg) {
    inline_ptr_call(qitem_alt, get_order_locker, pg);
  }
  uint32_t get_queue_token() const {
    inline_ptr_call(qitem_alt, get_queue_token);
  }
  const spg_t& get_ordering_token() const {
    inline_ptr_call(qitem_alt, get_ordering_token);
  }
  using op_type_t = OpQueueable::op_type_t;
  //OpQueueable::op_type_t __attribute__((noinline)) get_op_type() const {
  OpQueueable::op_type_t get_op_type() const {
    inline_ptr_call(qitem_alt, get_op_type);
  }

  boost::optional<OpRequestRef> maybe_get_op() const {
    inline_ptr_call(qitem_alt, maybe_get_op);
  }
  uint64_t get_reserved_pushes() const {
    inline_ptr_call(qitem_alt, get_reserved_pushes);
  }
  void run(OSD *osd, OSDShard *sdata,PGRef& pg, ThreadPool::TPHandle &handle) {
    inline_ptr_call(qitem_alt, run, osd, sdata, pg, handle);
  }
  unsigned get_priority() const { return priority; }
  int get_cost() const { return cost; }
  utime_t get_start_time() const { return start_time; }
  uint64_t get_owner() const { return owner; }
  epoch_t get_map_epoch() const { return map_epoch; }

  bool is_peering() const {
    inline_ptr_call(qitem_alt, is_peering);
  }

  const PGCreateInfo *creates_pg() const {
    inline_ptr_call(qitem_alt, creates_pg);
  }

  bool peering_requires_pg() const {
    inline_ptr_call(qitem_alt, peering_requires_pg);
  }

  friend ostream& operator<<(ostream& out, const OpQueueItem& item) {
     out << "OpQueueItem("
	 //<< item.get_ordering_token() << " " << *item.qitem
	 << " prio " << item.get_priority()
	 << " cost " << item.get_cost()
	 << " e" << item.get_map_epoch();
     if (item.get_reserved_pushes()) {
       out << " reserved_pushes " << item.get_reserved_pushes();
     }
    return out << ")";
  }
}; // class OpQueueItem

class PGPeeringItem : public PGOpQueueable {
  PGPeeringEventRef evt;
public:
  PGPeeringItem(spg_t pg, PGPeeringEventRef e) : PGOpQueueable(pg), evt(e) {}
  op_type_t get_op_type() const override final {
    return op_type_t::peering_event;
  }
  ostream &print(ostream &rhs) const override final {
    return rhs << "PGPeeringEvent(" << evt->get_desc() << ")";
  }
  void run(OSD *osd, OSDShard *sdata, PGRef& pg, ThreadPool::TPHandle &handle) override final;
  bool is_peering() const override {
    return true;
  }
  bool peering_requires_pg() const override {
    return evt->requires_pg;
  }
  const PGCreateInfo *creates_pg() const override {
    return evt->create_info.get();
  }
};

class PGSnapTrim : public PGOpQueueable {
  epoch_t epoch_queued;
public:
  PGSnapTrim(
    spg_t pg,
    epoch_t epoch_queued)
    : PGOpQueueable(pg), epoch_queued(epoch_queued) {}
  op_type_t get_op_type() const override final {
    return op_type_t::bg_snaptrim;
  }
  ostream &print(ostream &rhs) const override final {
    return rhs << "PGSnapTrim(pgid=" << get_pgid()
	       << "epoch_queued=" << epoch_queued
	       << ")";
  }
  void run(
    OSD *osd, OSDShard *sdata, PGRef& pg, ThreadPool::TPHandle &handle) override final;
};

class PGScrub : public PGOpQueueable {
  epoch_t epoch_queued;
public:
  PGScrub(
    spg_t pg,
    epoch_t epoch_queued)
    : PGOpQueueable(pg), epoch_queued(epoch_queued) {}
  op_type_t get_op_type() const override final {
    return op_type_t::bg_scrub;
  }
  ostream &print(ostream &rhs) const override final {
    return rhs << "PGScrub(pgid=" << get_pgid()
	       << "epoch_queued=" << epoch_queued
	       << ")";
  }
  void run(
    OSD *osd, OSDShard *sdata, PGRef& pg, ThreadPool::TPHandle &handle) override final;
};

class PGRecovery : public PGOpQueueable {
  epoch_t epoch_queued;
  uint64_t reserved_pushes;
public:
  PGRecovery(
    spg_t pg,
    epoch_t epoch_queued,
    uint64_t reserved_pushes)
    : PGOpQueueable(pg),
      epoch_queued(epoch_queued),
      reserved_pushes(reserved_pushes) {}
  op_type_t get_op_type() const override final {
    return op_type_t::bg_recovery;
  }
  virtual ostream &print(ostream &rhs) const override final {
    return rhs << "PGRecovery(pgid=" << get_pgid()
	       << "epoch_queued=" << epoch_queued
	       << "reserved_pushes=" << reserved_pushes
	       << ")";
  }
  virtual uint64_t get_reserved_pushes() const override final {
    return reserved_pushes;
  }
  virtual void run(
    OSD *osd, OSDShard *sdata, PGRef& pg, ThreadPool::TPHandle &handle) override final;
};

class PGRecoveryContext : public PGOpQueueable {
  unique_ptr<GenContext<ThreadPool::TPHandle&>> c;
  epoch_t epoch;
public:
  PGRecoveryContext(spg_t pgid,
		    GenContext<ThreadPool::TPHandle&> *c, epoch_t epoch)
    : PGOpQueueable(pgid),
      c(c), epoch(epoch) {}
  op_type_t get_op_type() const override final {
    return op_type_t::bg_recovery;
  }
  ostream &print(ostream &rhs) const override final {
    return rhs << "PGRecoveryContext(pgid=" << get_pgid()
	       << " c=" << c.get() << " epoch=" << epoch
	       << ")";
  }
  void run(
    OSD *osd, OSDShard *sdata, PGRef& pg, ThreadPool::TPHandle &handle) override final;
};

class PGDelete : public PGOpQueueable {
  epoch_t epoch_queued;
public:
  PGDelete(
    spg_t pg,
    epoch_t epoch_queued)
    : PGOpQueueable(pg),
      epoch_queued(epoch_queued) {}
  op_type_t get_op_type() const override final {
    return op_type_t::bg_pg_delete;
  }
  ostream &print(ostream &rhs) const override final {
    return rhs << "PGDelete(" << get_pgid()
	       << " e" << epoch_queued
	       << ")";
  }
  void run(
    OSD *osd, OSDShard *sdata, PGRef& pg, ThreadPool::TPHandle &handle) override final;
};
