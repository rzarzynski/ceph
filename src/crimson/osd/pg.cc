// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "pg.h"

#include <functional>

#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <boost/range/algorithm/max_element.hpp>
#include <boost/range/numeric.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "messages/MOSDOp.h"
#include "messages/MOSDOpReply.h"
#include "messages/MOSDPGInfo.h"
#include "messages/MOSDPGLog.h"
#include "messages/MOSDPGNotify.h"
#include "messages/MOSDPGQuery.h"
#include "messages/MOSDRepOp.h"
#include "messages/MOSDRepOpReply.h"

#include "osd/OSDMap.h"

#include "os/Transaction.h"

#include "crimson/net/Connection.h"
#include "crimson/net/Messenger.h"
#include "crimson/os/futurized_collection.h"
#include "os/Transaction.h"
#include "crimson/os/cyan_store.h"

#include "crimson/osd/exceptions.h"
#include "crimson/osd/pg_meta.h"
#include "crimson/osd/pg_backend.h"
#include "crimson/osd/ops_executer.h"
#include "crimson/osd/osd_operations/peering_event.h"

namespace {
  seastar::logger& logger() {
    return ceph::get_logger(ceph_subsys_osd);
  }
}

namespace ceph::osd {

using ceph::common::local_conf;

class RecoverablePredicate : public IsPGRecoverablePredicate {
public:
  bool operator()(const set<pg_shard_t> &have) const override {
    return !have.empty();
  }
};

class ReadablePredicate: public IsPGReadablePredicate {
  pg_shard_t whoami;
public:
  explicit ReadablePredicate(pg_shard_t whoami) : whoami(whoami) {}
  bool operator()(const set<pg_shard_t> &have) const override {
    return have.count(whoami);
  }
};

PG::PG(
  spg_t pgid,
  pg_shard_t pg_shard,
  ceph::os::CollectionRef coll_ref,
  pg_pool_t&& pool,
  std::string&& name,
  cached_map_t osdmap,
  ShardServices &shard_services,
  ec_profile_t profile)
  : pgid{pgid},
    pg_whoami{pg_shard},
    coll_ref{coll_ref},
    pgmeta_oid{pgid.make_pgmeta_oid()},
    osdmap_gate("PG::osdmap_gate", std::nullopt),
    shard_services{shard_services},
    osdmap{osdmap},
    backend(
      PGBackend::create(
	pgid.pgid,
	pg_shard,
	pool,
	coll_ref,
	shard_services,
	profile)),
    peering_state(
      shard_services.get_cct(),
      pg_shard,
      pgid,
      PGPool(
	shard_services.get_cct(),
	osdmap,
	pgid.pool(),
	pool,
	osdmap->get_pool_name(pgid.pool())),
      osdmap,
      this,
      this)
{
  peering_state.set_backend_predicates(
    new ReadablePredicate(pg_whoami),
    new RecoverablePredicate());
  osdmap_gate.got_map(osdmap->get_epoch());
}

PG::~PG() {}

bool PG::try_flush_or_schedule_async() {
  (void)shard_services.get_store().do_transaction(
    coll_ref,
    ObjectStore::Transaction()).then(
      [this, epoch=get_osdmap_epoch()]() {
	return shard_services.start_operation<LocalPeeringEvent>(
	  this,
	  shard_services,
	  pg_whoami,
	  pgid,
	  epoch,
	  epoch,
	  PeeringState::IntervalFlush());
      });
  return false;
}

void PG::on_activate(interval_set<snapid_t>)
{
  projected_last_update = peering_state.get_info().last_update;
}

void PG::on_activate_complete()
{
  active_promise.set_value();
  active_promise = {};

  if (peering_state.needs_recovery()) {
    shard_services.start_operation<LocalPeeringEvent>(
      this,
      shard_services,
      pg_whoami,
      pgid,
      get_osdmap_epoch(),
      get_osdmap_epoch(),
      PeeringState::DoRecovery{});
  } else if (peering_state.needs_backfill()) {
    shard_services.start_operation<LocalPeeringEvent>(
      this,
      shard_services,
      pg_whoami,
      pgid,
      get_osdmap_epoch(),
      get_osdmap_epoch(),
      PeeringState::RequestBackfill{});
  } else {
    shard_services.start_operation<LocalPeeringEvent>(
      this,
      shard_services,
      pg_whoami,
      pgid,
      get_osdmap_epoch(),
      get_osdmap_epoch(),
      PeeringState::AllReplicasRecovered{});
  }
}

void PG::log_state_enter(const char *state) {
  logger().info("Entering state: {}", state);
}

void PG::log_state_exit(
  const char *state_name, utime_t enter_time,
  uint64_t events, utime_t event_dur) {
  logger().info(
    "Exiting state: {}, entered at {}, {} spent on {} events",
    state_name,
    enter_time,
    event_dur,
    events);
}

ceph::signedspan PG::get_mnow()
{
  return shard_services.get_mnow();
}

HeartbeatStampsRef PG::get_hb_stamps(int peer)
{
  return shard_services.get_hb_stamps(peer);
}

void PG::init(
  ceph::os::CollectionRef coll,
  int role,
  const vector<int>& newup, int new_up_primary,
  const vector<int>& newacting, int new_acting_primary,
  const pg_history_t& history,
  const PastIntervals& pi,
  bool backfill,
  ObjectStore::Transaction &t)
{
  coll_ref = coll;
  peering_state.init(
    role, newup, new_up_primary, newacting,
    new_acting_primary, history, pi, backfill, t);
}

seastar::future<> PG::read_state(ceph::os::FuturizedStore* store)
{
  return store->open_collection(coll_t(pgid)).then([this, store](auto ch) {
    coll_ref = ch;
    return PGMeta{store, pgid}.load();
  }).then([this, store](pg_info_t pg_info, PastIntervals past_intervals) {
    return peering_state.init_from_disk_state(
	std::move(pg_info),
	std::move(past_intervals),
	[this, store, &pg_info] (PGLog &pglog) {
	  return pglog.read_log_and_missing_crimson(
	    *store,
	    coll_ref,
	    peering_state.get_info(),
	    pgmeta_oid);
      });
  }).then([this, store]() {
    int primary, up_primary;
    vector<int> acting, up;
    peering_state.get_osdmap()->pg_to_up_acting_osds(
	pgid.pgid, &up, &up_primary, &acting, &primary);
    peering_state.init_primary_up_acting(
	up,
	acting,
	up_primary,
	primary);
    int rr = OSDMap::calc_pg_role(pg_whoami.osd, acting);
    if (peering_state.get_pool().info.is_replicated() || rr == pg_whoami.shard)
	peering_state.set_role(rr);
    else
	peering_state.set_role(-1);

    epoch_t epoch = get_osdmap_epoch();
    shard_services.start_operation<LocalPeeringEvent>(
	this,
	shard_services,
	pg_whoami,
	pgid,
	epoch,
	epoch,
	PeeringState::Initialize());

    return seastar::now();
  });
}

void PG::do_peering_event(
  const boost::statechart::event_base &evt,
  PeeringCtx &rctx)
{
  peering_state.handle_event(
    evt,
    &rctx);
  peering_state.write_if_dirty(rctx.transaction);
}

void PG::do_peering_event(
  PGPeeringEvent& evt, PeeringCtx &rctx)
{
  if (!peering_state.pg_has_reset_since(evt.get_epoch_requested())) {
    logger().debug("{} handling {}", __func__, evt.get_desc());
    do_peering_event(evt.get_event(), rctx);
  } else {
    logger().debug("{} ignoring {} -- pg has reset", __func__, evt.get_desc());
  }
}

void PG::handle_advance_map(
  cached_map_t next_map, PeeringCtx &rctx)
{
  vector<int> newup, newacting;
  int up_primary, acting_primary;
  next_map->pg_to_up_acting_osds(
    pgid.pgid,
    &newup, &up_primary,
    &newacting, &acting_primary);
  peering_state.advance_map(
    next_map,
    peering_state.get_osdmap(),
    newup,
    up_primary,
    newacting,
    acting_primary,
    rctx);
  osdmap_gate.got_map(next_map->get_epoch());
}

void PG::handle_activate_map(PeeringCtx &rctx)
{
  peering_state.activate_map(rctx);
}

void PG::handle_initialize(PeeringCtx &rctx)
{
  PeeringState::Initialize evt;
  peering_state.handle_event(evt, &rctx);
}


void PG::print(ostream& out) const
{
  out << peering_state << " ";
}


std::ostream& operator<<(std::ostream& os, const PG& pg)
{
  os << " pg_epoch " << pg.get_osdmap_epoch() << " ";
  pg.print(os);
  return os;
}

seastar::future<> PG::wait_for_active()
{
  logger().debug("wait_for_active: {}", peering_state.get_pg_state_string());
  if (peering_state.is_active()) {
    return seastar::now();
  } else {
    return active_promise.get_shared_future();
  }
}

seastar::future<> PG::submit_transaction(boost::local_shared_ptr<ObjectState>&& os,
					 ceph::os::Transaction&& txn,
					 const MOSDOp& req)
{
  epoch_t map_epoch = get_osdmap_epoch();
  eversion_t at_version{map_epoch, projected_last_update.version + 1};
  return backend->mutate_object(peering_state.get_acting_recovery_backfill(),
				std::move(os),
				std::move(txn),
				req,
				peering_state.get_last_peering_reset(),
				map_epoch,
				at_version).then([this](auto acked) {
    for (const auto& peer : acked) {
      peering_state.update_peer_last_complete_ondisk(
        peer.shard, peer.last_complete_ondisk);
    }
    return seastar::now();
  });
}

seastar::future<Ref<MOSDOpReply>> PG::do_osd_ops(Ref<MOSDOp> m)
{
  const auto oid = m->get_snapid() == CEPH_SNAPDIR ? m->get_hobj().get_head()
                                                   : m->get_hobj();
  return backend->get_object_state(oid).then([this, m](auto os) mutable {
    return ceph::do_with(OpsExecuter{std::move(os), *this/* as const& */, m},
                         [this, m] (auto& ox) {
      return ceph::do_for_each(m->ops, [this, &ox](OSDOp& osd_op) {
        logger().debug("will be handling op {}", ceph_osd_op_name(osd_op.op.op));
        return ox.execute_osd_op(osd_op);
      }).safe_then([this, m, &ox] {
        logger().debug("all operations have been executed successfully");
        return std::move(ox).submit_changes([this, m] (auto&& txn, auto&& os) {
          // XXX: the entire lambda could be scheduled conditionally. ::if_then()?
	  if (txn.empty()) {
            logger().debug("txn is empty, bypassing mutate");
	    return seastar::now();
	  } else {
	    return submit_transaction(std::move(os), std::move(txn), *m);
	  }
        });
      }, OpsExecuter::osd_op_errorator::pass_further{});
    }).safe_then([m,this] {
      auto reply = make_message<MOSDOpReply>(m.get(), 0, get_osdmap_epoch(),
                                             0, false);
      reply->add_flags(CEPH_OSD_FLAG_ACK | CEPH_OSD_FLAG_ONDISK);
      return seastar::make_ready_future<Ref<MOSDOpReply>>(std::move(reply));
    }, OpsExecuter::osd_op_errorator::all_same_way([] (const std::error_code& err) {
      assert(err.value() > 0);
      throw ceph::osd::make_error(err.value());
    }));
  }).handle_exception_type([=,&oid](const ceph::osd::error& e) {
    logger().debug("got ceph::osd::error while handling object {}: {} ({})",
                   oid, e.code(), e.what());
    return backend->evict_object_state(oid).then([=] {
      auto reply = make_message<MOSDOpReply>(
        m.get(), -e.code().value(), get_osdmap_epoch(), 0, false);
      reply->set_enoent_reply_versions(peering_state.get_info().last_update,
                                         peering_state.get_info().last_user_version);
      return seastar::make_ready_future<Ref<MOSDOpReply>>(std::move(reply));
    });
  });
}

seastar::future<Ref<MOSDOpReply>> PG::do_pg_ops(Ref<MOSDOp> m)
{
  return seastar::do_with(OpsExecuter{*this/* as const& */, m},
                          [this, m] (auto& ox) {
    return seastar::do_for_each(m->ops, [this, &ox](OSDOp& osd_op) {
      logger().debug("will be handling pg op {}", ceph_osd_op_name(osd_op.op.op));
      return ox.execute_pg_op(osd_op);
    });
  }).then([m, this] {
    auto reply = make_message<MOSDOpReply>(m.get(), 0, get_osdmap_epoch(),
                                           CEPH_OSD_FLAG_ACK | CEPH_OSD_FLAG_ONDISK,
                                           false);
    return seastar::make_ready_future<Ref<MOSDOpReply>>(std::move(reply));
  }).handle_exception_type([=](const ceph::osd::error& e) {
    auto reply = make_message<MOSDOpReply>(
      m.get(), -e.code().value(), get_osdmap_epoch(), 0, false);
    reply->set_enoent_reply_versions(peering_state.get_info().last_update,
				     peering_state.get_info().last_user_version);
    return seastar::make_ready_future<Ref<MOSDOpReply>>(std::move(reply));
  });
}

seastar::future<> PG::handle_op(ceph::net::Connection* conn,
                                Ref<MOSDOp> m)
{
  return wait_for_active().then([conn, m, this] {
    if (m->finish_decode()) {
      m->clear_payload();
    }
    if (std::any_of(begin(m->ops), end(m->ops),
		    [](auto& op) { return ceph_osd_op_type_pg(op.op.op); })) {
      return do_pg_ops(m);
    } else {
      return do_osd_ops(m);
    }
  }).then([conn](Ref<MOSDOpReply> reply) {
    return conn->send(reply);
  });
}

seastar::future<> PG::handle_rep_op(Ref<MOSDRepOp> req)
{
  ceph::os::Transaction txn;
  auto encoded_txn = req->get_data().cbegin();
  decode(txn, encoded_txn);
  return shard_services.get_store().do_transaction(coll_ref, std::move(txn))
    .then([req, lcod=peering_state.get_info().last_complete, this] {
      peering_state.update_last_complete_ondisk(lcod);
      const auto map_epoch = get_osdmap_epoch();
      auto reply = make_message<MOSDRepOpReply>(
        req.get(), pg_whoami, 0,
	map_epoch, req->get_min_epoch(), CEPH_OSD_FLAG_ONDISK);
      reply->set_last_complete_ondisk(lcod);
      return shard_services.send_to_osd(req->from.osd, reply, map_epoch);
    });
}

void PG::handle_rep_op_reply(ceph::net::Connection* conn,
			     const MOSDRepOpReply& m)
{
  backend->got_rep_op_reply(m);
}

}
