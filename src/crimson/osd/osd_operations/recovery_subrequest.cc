#include <fmt/format.h>
#include <fmt/ostream.h>

#include "crimson/osd/osd_operations/recovery_subrequest.h"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_osd);
  }
}

namespace crimson::osd {

seastar::future<> RecoverySubRequest::start() {
  logger().debug("{}: start", *this);

  IRef opref = this;
  return with_blocking_future(osd.osdmap_gate.wait_for_map(m->get_min_epoch()))
  .then([this] (epoch_t epoch) {
    return with_blocking_future(osd.wait_for_pg(m->get_spg()));
  }).then([this, opref=std::move(opref)] (Ref<PG> pgref) {
    return seastar::do_with(std::move(pgref), std::move(opref),
      [this](auto& pgref, auto& opref) {
      switch (m->get_header().type) {
      case MSG_OSD_PG_PULL:
	return pgref->handle_pull(boost::static_pointer_cast<MOSDPGPull>(m));
      case MSG_OSD_PG_PUSH:
	return pgref->handle_push(boost::static_pointer_cast<MOSDPGPush>(m));
      case MSG_OSD_PG_PUSH_REPLY:
	return pgref->handle_push_reply(
	    boost::static_pointer_cast<MOSDPGPushReply>(m));
      case MSG_OSD_PG_RECOVERY_DELETE:
	return pgref->handle_recovery_delete(
	    boost::static_pointer_cast<MOSDPGRecoveryDelete>(m));
      case MSG_OSD_PG_RECOVERY_DELETE_REPLY:
	return pgref->handle_recovery_delete_reply(
	    boost::static_pointer_cast<MOSDPGRecoveryDeleteReply>(m));
      default:
	return seastar::make_exception_future<>(
	    std::runtime_error(fmt::format("invalid request type: {}",
					  m->get_header().type)));
      }
    });
  });
}

}
