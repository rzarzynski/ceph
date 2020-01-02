// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "crimson/osd/watch.h"
#include "messages/MWatchNotify.h"


namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_osd);
  }
}

namespace crimson::osd {

bool Watch::NotifyCmp::operator()(NotifyRef lhs, NotifyRef rhs) const
{
  ceph_assert(lhs);
  ceph_assert(rhs);
  return lhs->get_id() < rhs->get_id();
}

Watch::~Watch() {
  logger().info("{}: destructing Watch (cookie={})", __func__, winfo.cookie);
}

seastar::future<> Watch::connect(crimson::net::ConnectionRef conn, bool)
{
  if (this->conn == conn) {
    logger().debug("conn={} already connected", conn);
  }

  this->conn = std::move(conn);
  return seastar::now();
}

seastar::future<> Watch::send_notify_msg(NotifyRef notify)
{
  logger().info("{} for notify(id={})", __func__, notify->ninfo.notify_id);
  return conn->send(make_message<MWatchNotify>(
    winfo.cookie,
    notify->user_version,
    notify->ninfo.notify_id,
    CEPH_WATCH_EVENT_NOTIFY,
    notify->ninfo.bl,
    notify->client_gid));
}

seastar::future<> Watch::start_notify(NotifyRef notify)
{
  logger().info("{} adding &notify={}", __func__, notify.get());
  auto [ it, emplaced ] = in_progress_notifies.emplace(std::move(notify));
  ceph_assert(emplaced);
  ceph_assert(is_alive());
  return is_connected() ? send_notify_msg(*it) : seastar::now();
}

seastar::future<> Watch::notify_ack(
  const uint64_t notify_id,
  const ceph::bufferlist& reply_bl)
{
  logger().info("{}", __func__);
  return seastar::do_for_each(in_progress_notifies,
    [this_shared=shared_from_this(), &reply_bl] (auto notify) {
      return notify->complete_watcher(this_shared, reply_bl);
    }
  ).then([this] {
    in_progress_notifies.clear();
    return seastar::now();
  });
}

seastar::future<> Watch::send_disconnect_msg()
{
  if (!is_connected()) {
    return seastar::now();
  }
  ceph::bufferlist empty;
  return conn->send(make_message<MWatchNotify>(
    winfo.cookie,
    0,
    0,
    CEPH_WATCH_EVENT_DISCONNECT,
    empty));
}

void Watch::discard_state()
{
  ceph_assert(obc);
  in_progress_notifies.clear();
}

seastar::future<> Watch::remove(const bool send_disconnect)
{
  logger().info("{}", __func__);
  auto disconnected = send_disconnect ? send_disconnect_msg()
                                      : seastar::now();
  return std::move(disconnected).then([this] {
    return seastar::do_for_each(in_progress_notifies,
      [this_shared=shared_from_this()] (auto notify) {
        return notify->remove_watcher(this_shared);
      }).then([this] {
        discard_state();
        return seastar::now();
      });
    });
}

seastar::future<> Notify::remove_watcher(WatchRef watch)
{
  if (discarded || complete) {
    return seastar::now();
  }
  [[maybe_unused]] const auto num_removed = watchers.erase(watch);
  ceph_assert(num_removed > 0);
  return maybe_send_completion();
}

seastar::future<> Notify::complete_watcher(
  WatchRef watch,
  const ceph::bufferlist& reply_bl)
{
  if (discarded || complete) {
    return seastar::now();
  }
#if 0
  notify_replies.emplace_back(watch->get_watcher_gid(),
                              watch->get_cookie(),
                              reply_bl);
#else
  notify_replies.insert(make_pair(make_pair(watch->get_watcher_gid(),
					    watch->get_cookie()),
				  reply_bl));
#endif
  return remove_watcher(std::move(watch));
}

seastar::future<> Notify::maybe_send_completion()
{
  logger().info("{} -- {} in progress watchers", __func__, watchers.size());
  if (watchers.empty()) {
    // prepare reply
    ceph::bufferlist bl;
    encode(notify_replies, bl);
    // FIXME: this is just a stub
    std::list<std::pair<uint64_t,uint64_t>> missed;
    encode(missed, bl);

    complete = true;

    ceph::bufferlist empty;
    auto reply = make_message<MWatchNotify>(
      ninfo.cookie,
      user_version,
      ninfo.notify_id,
      CEPH_WATCH_EVENT_NOTIFY_COMPLETE,
      empty,
      client_gid);
    reply->set_data(bl);
    return conn->send(std::move(reply));
  }
  return seastar::now();
}

} // namespace crimson::osd
