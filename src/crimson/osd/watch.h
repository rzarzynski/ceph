// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <memory>

#include "crimson/net/Connection.h"

namespace crimson::osd {

class Watch : private std::enable_shared_from_this<Watch> {
public:
  Watch() = default;

public:
  seastar::future<> connect(crimson::net::ConnectionRef, bool) {
    return seastar::now();
  }
  bool is_connected() const {
    return true;
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

} // namespace crimson::osd
