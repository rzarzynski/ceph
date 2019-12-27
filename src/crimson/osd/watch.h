// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <iterator>
#include <memory>
#include <set>

#include "crimson/net/Connection.h"
#include "crimson/osd/object_context.h"

namespace crimson::osd {

class Notify;
using NotifyRef = std::shared_ptr<Notify>;

// NOTE: really need to have this public. Otherwise `shared_from_this()`
// will abort. According to cppreference.com:
//
//   "The constructors of std::shared_ptr detect the presence
//   of an unambiguous and accessible (ie. public inheritance
//   is mandatory) (since C++17) enable_shared_from_this base".
class Watch : public std::enable_shared_from_this<Watch> {
  struct NotifyCmp {
    inline bool operator()(NotifyRef lhs, NotifyRef rhs) const;
  };
  ::std::set<NotifyRef, NotifyCmp> in_progress_notifies;
  ::crimson::net::ConnectionRef conn;
  ::crimson::osd::ObjectContextRef obc;

  watch_info_t winfo;

  seastar::future<> start_notify(NotifyRef);
  seastar::future<> send_notify_msg(NotifyRef);

  friend Notify;
public:
  // XXX: hack for make_shared
  Watch(const watch_info_t& winfo,
        ::crimson::osd::ObjectContextRef obc)
    : winfo(winfo),
      obc(std::move(obc)) {
  }

public:
  seastar::future<> connect(crimson::net::ConnectionRef, bool);
  bool is_alive() const {
    return true;
  }
  bool is_connected() const {
    return static_cast<bool>(conn);
  }
  void got_ping(utime_t) {
    // NOP
  }

  seastar::future<> remove(bool) {
    return seastar::now();
  }

  /// Call when notify_ack received on notify_id
  seastar::future<> notify_ack(
    uint64_t notify_id, ///< [in] id of acked notify
    const ceph::bufferlist& reply_bl) { ///< [in] notify reply buffer
    return seastar::now();
  }

  // TODO: we don't need the atomical ref-counting but shared_from_this
  // will be useful. Maybe switch to something from boost later.
  using Ref = std::shared_ptr<Watch>;

  template <class... Args>
  static std::shared_ptr<Watch> create(Args&&... args) {
    return std::make_shared<Watch>(std::forward<Args>(args)...);
  };
};

using WatchRef = Watch::Ref;

class Notify {
  std::set<WatchRef> watchers;
  notify_info_t ninfo;
  uint64_t client_gid;
  uint64_t user_version;

  uint64_t get_id() { return ninfo.notify_id; }
  seastar::future<> propagate() { return seastar::now(); }

  template <class WatchIteratorT>
  Notify(WatchIteratorT begin,
         WatchIteratorT end,
         const notify_info_t& ninfo,
         const uint64_t client_gid,
         const uint64_t user_version);
  // this is a private tag for the public constructor that turns it into
  // de facto private one. The motivation behind the hack is make_shared
  // used by create_n_propagate factory.
  struct private_ctag_t{};

  friend Watch;

public:
  template <class... Args>
  Notify(private_ctag_t, Args&&... args) : Notify(std::forward<Args>(args)...) {
  }

  template <class WatchIteratorT, class... Args>
  static seastar::future<> create_n_propagate(
    WatchIteratorT begin,
    WatchIteratorT end,
    Args&&... args);
};


template <class WatchIteratorT>
Notify::Notify(WatchIteratorT begin,
               WatchIteratorT end,
               const notify_info_t& ninfo,
               const uint64_t client_gid,
               const uint64_t user_version)
  : ninfo(ninfo),
    client_gid(client_gid),
    user_version(user_version)
{
  std::copy(begin, end, std::inserter(watchers, std::begin(watchers)));
}

template <class WatchIteratorT, class... Args>
seastar::future<> Notify::create_n_propagate(
  WatchIteratorT begin,
  WatchIteratorT end,
  Args&&... args)
{
  static_assert(
    std::is_same_v<typename std::iterator_traits<WatchIteratorT>::value_type,
                   crimson::osd::WatchRef>);
  auto notify = std::make_shared<Notify>(
    private_ctag_t{},
    begin,
    end,
    std::forward<Args>(args)...);
  return seastar::do_for_each(begin, end, [=] (auto& watchref) {
    return watchref->start_notify(notify);
  }).then([notify = std::move(notify)] {
    return notify->propagate();
  });
}

} // namespace crimson::osd
