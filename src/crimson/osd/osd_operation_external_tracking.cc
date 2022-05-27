// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/config.h"
#include "crimson/osd/osd.h"
#include "crimson/osd/osd_operation_external_tracking.h"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_osd);
  }
}

namespace crimson::osd {

void HistoricBackend::handle(ClientRequest::CompletionEvent&,
                             const Operation& op)
{
  // early exit if the history is disabled
  using crimson::common::local_conf;
  if (!local_conf()->osd_op_history_size) {
    return;
  }

#ifdef NDEBUG
  const auto& client_request = static_cast<const ClientRequest&>(op);
#else
  const auto& client_request = dynamic_cast<const ClientRequest&>(op);
#endif
  auto& main_registry = client_request.osd.get_shard_services().registry;

  // unlink the op from the client request registry. this is a part of
  // the re-link procedure. finally it will be in historic registry.
  constexpr auto client_reg_index =
    static_cast<size_t>(OperationTypeCode::client_request);
  constexpr auto historic_reg_index =
    static_cast<size_t>(OperationTypeCode::historic_client_request);
  auto& client_registry = main_registry.get_registry<client_reg_index>();
  auto& historic_registry = main_registry.get_registry<historic_reg_index>();

  historic_registry.splice(std::end(historic_registry),
			   client_registry,
			   client_registry.iterator_to(client_request));
  ClientRequest::ICRef(
    &client_request, /* add_ref= */true
  ).detach(); // yes, "leak" it for now!
  static std::decay_t<decltype(historic_registry)>::const_iterator last_of_recents {
    std::begin(historic_registry)
  };
  static size_t num_recent_ops = 0;
  static size_t num_slow_ops = 0;
  ++num_recent_ops;

  // check whether the history size limit is not exceeded; if so, then
  // purge the oldest op.
  // NOTE: Operation uses the auto-unlink feature of boost::intrusive.
  // NOTE: the cleaning happens in OSDOperationRegistry::do_stop()
  if (num_recent_ops > local_conf()->osd_op_history_size) {
    ++last_of_recents;
		++num_slow_ops;
  }
	if (num_slow_ops > local_conf()->osd_op_history_slow_op_size) {
		// we're interested in keeping slowest ops. if the slow op history
		// is disabled, the list will have only one element the full-blown
		// search will boil down into `.front()`.
    const auto fastest_historic_iter = std::min_element(
			std::cbegin(historic_registry), last_of_recents,
			[] (const auto& lop, const auto& rop) {
        const auto& lclient_request = static_cast<const ClientRequest&>(lop);
        const auto& rclient_request = static_cast<const ClientRequest&>(rop);
        const auto lduration =
				  lclient_request.get_completed() - lclient_request.get_started();
        const auto rduration =
				  rclient_request.get_completed() - rclient_request.get_started();
			  return lduration < rduration;
			});
		assert(fastest_historic_iter != std::end(historic_registry));
		const auto& fastest_historic_op =
      static_cast<const ClientRequest&>(*fastest_historic_iter);
    // clear a previously "leaked" op
    ClientRequest::ICRef(&fastest_historic_op, /* add_ref= */false);
	}
}

} // namespace crimson::osd
