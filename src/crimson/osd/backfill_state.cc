// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "crimson/osd/backfill_state.h"
#include "crimson/osd/backfill_facades.h"
#include "crimson/osd/pg.h"
#include "osd/PeeringState.h"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_osd);
  }
}

namespace crimson::osd {

BackfillState::BackfillState(
  BackfillState::BackfillListener& backfill_listener,
  std::unique_ptr<BackfillState::PeeringFacade> peering_state,
  std::unique_ptr<BackfillState::PGFacade> pg)
  : backfill_machine(*this,
                     backfill_listener,
                     std::move(peering_state),
                     std::move(pg)),
    progress_tracker(
      std::make_unique<BackfillState::ProgressTracker>(backfill_machine))
{
  logger().debug("{}:{}", __func__, __LINE__);
  backfill_machine.initiate();
}

BackfillState::~BackfillState() = default;

BackfillState::BackfillMachine::BackfillMachine(
  BackfillState& backfill_state,
  BackfillState::BackfillListener& backfill_listener,
  std::unique_ptr<BackfillState::PeeringFacade> peering_state,
  std::unique_ptr<BackfillState::PGFacade> pg)
  : backfill_state(backfill_state),
    backfill_listener(backfill_listener),
    peering_state(std::move(peering_state)),
    pg(std::move(pg))
{}

BackfillState::BackfillMachine::~BackfillMachine() = default;

BackfillState::Initial::Initial(my_context ctx)
  : my_base(ctx)
{
  bs().last_backfill_started = ps().earliest_backfill();
  logger().debug("{}: bft={} from {}",
                 __func__, ps().get_backfill_targets(),
                 bs().last_backfill_started);
  for (const auto& bt : ps().get_backfill_targets()) {
    logger().debug("{}: target shard {} from {}",
                   __func__, bt, ps().get_peer_info(bt).last_backfill);
  }
  ceph_assert(ps().get_backfill_targets().size());
  ceph_assert(!bs().last_backfill_started.is_max());
}

boost::statechart::result
BackfillState::Initial::react(const BackfillState::Trigerred& evt)
{
  ceph_assert(bs().last_backfill_started == ps().earliest_backfill());
  // initialize BackfillIntervals
  for (const auto& bt : ps().get_backfill_targets()) {
    bs().peer_backfill_info[bt].reset(
      ps().get_peer_info(bt).last_backfill);
  }
  bs().backfill_info.reset(bs().last_backfill_started);
  bs().backfills_in_flight.clear();
  bs().pending_backfill_updates.clear();
  if (Enqueuing::all_enqueued(ps(),
                              bs().backfill_info,
                              bs().peer_backfill_info)) {
    return transit<BackfillState::Done>();
  } else {
    return transit<BackfillState::Enqueuing>();
  }
}


// -- Enqueuing
void BackfillState::Enqueuing::maybe_update_range()
{
  if (bs().backfill_info.version >= pg().get_projected_last_update()) {
    logger().info("{}: bi is current", __func__);
    ceph_assert(bs().backfill_info.version == pg().get_projected_last_update());
  } else if (bs().backfill_info.version >= ps().get_info().log_tail) {
#if 0
    if (ps().get_pg_log().get_log().empty() &&
        pg().get_projected_log().empty()) {
      /* Because we don't move log_tail on split, the log might be
       * empty even if log_tail != last_update.  However, the only
       * way to get here with an empty log is if log_tail is actually
       * eversion_t(), because otherwise the entry which changed
       * last_update since the last scan would have to be present.
       */
      ceph_assert(bs().backfill_info.version == eversion_t());
      return;
    }
#endif
    logger().debug("{}: bi is old, ({}) can be updated with log to {}",
                   __func__,
                   bs().backfill_info.version,
                   pg().get_projected_last_update());
    logger().debug("{}: scanning pg log first", __func__);
    ps().get_pg_log().get_log().scan_log_after(bs().backfill_info.version,
      [&, this](const pg_log_entry_t& e) {
        logger().debug("maybe_update_range(lambda): updating from version {}",
                       e.version);
        if (e.soid >= bs().backfill_info.begin &&
	    e.soid <  bs().backfill_info.end) {
	  if (e.is_update()) {
	    logger().debug("maybe_update_range(lambda): {} updated to ver {}",
                           e.soid, e.version);
            bs().backfill_info.objects.erase(e.soid);
            bs().backfill_info.objects.insert(std::make_pair(e.soid,
                                                             e.version));
	  } else if (e.is_delete()) {
            logger().debug("maybe_update_range(lambda): {} removed",
                           e.soid);
            bs().backfill_info.objects.erase(e.soid);
          }
        }
      });
    bs().backfill_info.version = pg().get_projected_last_update();
  } else {
    ceph_abort_msg("scan_range should have raised bs().backfill_info.version past log_tail");
  }
}

void BackfillState::Enqueuing::trim_backfill_infos()
{
  for (const auto& bt : ps().get_backfill_targets()) {
    bs().peer_backfill_info[bt].trim_to(
      std::max(ps().get_peer_info(bt).last_backfill,
               bs().last_backfill_started));
  }
  bs().backfill_info.trim_to(bs().last_backfill_started);
}

/* static */ bool BackfillState::Enqueuing::all_peer_enqueued(
  const PeeringFacade& ps,
  const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info)
{
  for (const auto& bt : ps.get_backfill_targets()) {
    const auto piter = peer_backfill_info.find(bt);
    ceph_assert(piter != peer_backfill_info.end());
    const BackfillInterval& pbi = piter->second;
    // See if peer has more to process
    if (!pbi.extends_to_end() || !pbi.empty()) {
      return false;
    }
  }
  return true;
}

bool BackfillState::Enqueuing::all_peer_enqueued(
  const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info) const
{
  // Primary hasn't got any more objects
  ceph_assert(bs().backfill_info.empty());
  return all_peer_enqueued(ps(), peer_backfill_info);
}

/* static */ bool BackfillState::Enqueuing::all_enqueued(
  const PeeringFacade& ps,
  const BackfillInterval& backfill_info,
  const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info)
{
  const bool all_local_enqueued = \
    backfill_info.extends_to_end() && backfill_info.empty();
  const bool all_peer_enqueued =  \
    Enqueuing::all_peer_enqueued(ps, peer_backfill_info);
  return all_local_enqueued && all_peer_enqueued;
}

hobject_t BackfillState::Enqueuing::earliest_peer_backfill(
  const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info) const
{
  hobject_t e = hobject_t::get_max();
  for (const pg_shard_t& bt : ps().get_backfill_targets()) {
    const auto iter = peer_backfill_info.find(bt);
    ceph_assert(iter != peer_backfill_info.end());
    e = std::min(e, iter->second.begin);
  }
  return e;
}

bool BackfillState::Enqueuing::should_rescan_replicas(
  const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info,
  const BackfillInterval& backfill_info) const
{
  const auto& targets = ps().get_backfill_targets();
  return std::any_of(std::begin(targets), std::end(targets),
    [&, this] (const auto& bt) {
      return ReplicasScanning::replica_needs_scan(peer_backfill_info.at(bt),
                                                  backfill_info);
    });
}

bool BackfillState::Enqueuing::should_rescan_primary(
  const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info,
  const BackfillInterval& backfill_info) const
{
  return backfill_info.begin <= earliest_peer_backfill(peer_backfill_info) &&
	 backfill_info.extends_to_end() == false;
}

void BackfillState::Enqueuing::loop_advancer_t::advance(
  hobject_t& last_backfill_started,
  std::map<pg_shard_t, BackfillInterval>& peer_backfill_info) &&
{
  std::for_each(std::begin(pbi_targets), std::end(pbi_targets),
    [this, &peer_backfill_info] (const auto& bt) {
      peer_backfill_info.at(bt).pop_front();
    });
  last_backfill_started = std::move(new_last_backfill_started);
}

BackfillState::Enqueuing::loop_advancer_t
BackfillState::Enqueuing::remove_on_peers(const hobject_t& check)
{
  // set `new_last_backfill_started` to `check`
  loop_advancer_t result { {}, check };
  for (const auto& bt : ps().get_backfill_targets()) {
    const auto& pbi = bs().peer_backfill_info.at(bt);
    if (pbi.begin == check) {
      result.pbi_targets.insert(bt);
      const auto& version = pbi.objects.begin()->second;
      bs().progress_tracker->enqueue_drop(pbi.begin);
      ls().enqueue_drop(bt, pbi.begin, version);
    }
  }
  logger().debug("{}: BACKFILL removing {} from peers {}",
                 __func__, check, result.pbi_targets);
  ceph_assert(!result.pbi_targets.empty());
  return result;
}

BackfillState::Enqueuing::loop_advancer_t
BackfillState::Enqueuing::update_on_peers(const hobject_t& check)
{
  loop_advancer_t result { {}, bs().backfill_info.begin };
  const eversion_t& obj_v = bs().backfill_info.objects.begin()->second;

  for (const auto& bt : ps().get_backfill_targets()) {
    const auto& pbi = bs().peer_backfill_info.at(bt);

    // Find all check peers that have the wrong version
    if (check == bs().backfill_info.begin && check == pbi.begin) {
      if (pbi.objects.begin()->second != obj_v) {
        bs().progress_tracker->enqueue_push(bs().backfill_info.begin);
        ls().enqueue_push(bt, bs().backfill_info.begin, obj_v);
      } else {
        // it's fine, keep it!
      }
      result.pbi_targets.insert(bt);
    } else {
      const pg_info_t& pinfo = ps().get_peer_info(bt);
      // Only include peers that we've caught up to their backfill line
      // otherwise, they only appear to be missing this object
      // because their pbi.begin > backfill_info.begin.
      if (bs().backfill_info.begin > pinfo.last_backfill) {
        bs().progress_tracker->enqueue_push(bs().backfill_info.begin);
        ls().enqueue_push(bt, bs().backfill_info.begin, obj_v);
      }
    }
  }
  return result;
}

BackfillState::Enqueuing::Enqueuing(my_context ctx)
  : my_base(ctx)
{

  // update our local interval to cope with recent changes
  bs().backfill_info.begin = bs().last_backfill_started;
  if (bs().backfill_info.version < ps().get_info().log_tail) {
    logger().debug("{}: bi is old, rescanning of local backfill_info",
                   __func__);
    post_event(RequestPrimaryScanning{});
    return;
  } else {
    maybe_update_range();
  }
  trim_backfill_infos();

  while (!bs().backfill_info.empty()) {
    if (!ls().budget_available()) {
      post_event(RequestWaiting{});
      return;
    } else if (should_rescan_replicas(bs().peer_backfill_info,
                                      bs().backfill_info)) {
      // Count simultaneous scans as a single op and let those complete
      post_event(RequestReplicasScanning{});
      return;
    }
    // Get object within set of peers to operate on and  the set of targets
    // for which that object applies.
    const hobject_t check = earliest_peer_backfill(bs().peer_backfill_info);
    if (check < bs().backfill_info.begin) {
      // Don't increment ops here because deletions
      // are cheap and not replied to unlike real recovery_ops,
      // and we can't increment ops without requeueing ourself
      // for recovery.
      remove_on_peers(check).advance(bs().last_backfill_started,
                                     bs().peer_backfill_info);
    } else {
      update_on_peers(check).advance(bs().last_backfill_started,
                                     bs().peer_backfill_info);
      bs().backfill_info.pop_front();
    }
  }

  if (should_rescan_primary(bs().peer_backfill_info,
                            bs().backfill_info)) {
    // need to grab one another chunk of the object namespace and  restart
    // the queueing.
    logger().debug("{}: reached end for current local chunk",
                   __func__);
    post_event(RequestPrimaryScanning{});
  } else {
    logger().info("{}: reached end for both local and all peers.",
                  __func__);
    ceph_assert(!bs().progress_tracker->tracked_objects_completed());
    post_event(RequestWaiting{});
  }
}

// -- PrimaryScanning
BackfillState::PrimaryScanning::PrimaryScanning(my_context ctx)
  : my_base(ctx)
{
  bs().backfill_info.version = ps().get_info().last_update;
  ls().request_primary_scan(bs().backfill_info.begin);
}

boost::statechart::result
BackfillState::PrimaryScanning::react(PrimaryScanned evt)
{
  bs().backfill_info = std::move(evt.result);
  return transit<Enqueuing>();
}

boost::statechart::result
BackfillState::PrimaryScanning::react(ObjectPushed evt)
{
  bs().progress_tracker->complete_to(evt.object, evt.stat);
  return discard_event();
}

// -- ReplicasScanning
bool BackfillState::ReplicasScanning::replica_needs_scan(
  const BackfillInterval& replica_backfill_info,
  const BackfillInterval& local_backfill_info)
{
  return replica_backfill_info.empty() && \
         replica_backfill_info.begin <= local_backfill_info.begin && \
         replica_backfill_info.extends_to_end() == false;
}

BackfillState::ReplicasScanning::ReplicasScanning(my_context ctx)
  : my_base(ctx)
{
  for (const auto& bt : ps().get_backfill_targets()) {
    if (const auto& pbi = bs().peer_backfill_info.at(bt);
        replica_needs_scan(pbi, bs().backfill_info)) {
      logger().debug("{}: scanning peer osd.{} from {}",
                     __func__, bt, pbi.end);
      ls().request_replica_scan(bt, pbi.end, hobject_t{});

      ceph_assert(bs().waiting_on_backfill.find(bt) == \
                  bs().waiting_on_backfill.end());
      bs().waiting_on_backfill.insert(bt);
    }
  }
  ceph_assert(bs().waiting_on_backfill.empty() == false);
  // TODO: start_recovery_op(hobject_t::get_max()); // XXX: was pbi.end
}

#if 0
BackfillState::ReplicasScanning::~ReplicasScanning()
{
  // TODO: finish_recovery_op(hobject_t::get_max());
}
#endif

boost::statechart::result
BackfillState::ReplicasScanning::react(ReplicaScanned evt)
{
  // TODO: maybe we'll be able to move waiting_on_backfill from
  // the machine to the state.
  //ceph_assert(ps().is_backfill_target(evt.from));
  if (bs().waiting_on_backfill.erase(evt.from)) {
    bs().peer_backfill_info[evt.from] = std::move(evt.result);
    if (bs().waiting_on_backfill.empty()) {
      ceph_assert(
        bs().peer_backfill_info.size() == ps().get_backfill_targets().size());
      return transit<Enqueuing>();
    }
  } else {
    // we canceled backfill for a while due to a too full, and this
    // is an extra response from a non-too-full peer
    logger().debug("{}: canceled backfill (too full?)", __func__);
  }
  return discard_event();
}

boost::statechart::result
BackfillState::ReplicasScanning::react(ObjectPushed evt)
{
  bs().progress_tracker->complete_to(evt.object, evt.stat);
  return discard_event();
}

// -- Waiting
BackfillState::Waiting::Waiting(my_context ctx)
  : my_base(ctx)
{
  logger().debug("{}: entered Waiting", __func__);
}

boost::statechart::result
BackfillState::Waiting::react(ObjectPushed evt)
{
  logger().debug("Waiting::react() on ObjectPushed; evt.object={}",
                 evt.object);
  bs().progress_tracker->complete_to(evt.object, evt.stat);
  if (!Enqueuing::all_enqueued(ps(),
                               bs().backfill_info,
                               bs().peer_backfill_info)) {
    return transit<Enqueuing>();
  } else if (bs().progress_tracker->tracked_objects_completed()) {
    return transit<Done>();
  } else {
    // we still something to wait on
    logger().debug("Waiting::react() on ObjectPushed; still waiting");
    return discard_event();
  }
}

// -- Done
BackfillState::Done::Done(my_context ctx)
  : my_base(ctx)
{
  logger().debug("{}: signalling backfill is done", __func__);
  ls().backfilled();
}

// -- Crashed
BackfillState::Crashed::Crashed()
{
  ceph_assert("{}: this should not happen" == nullptr);
}

// -- ProgressTracker
bool BackfillState::ProgressTracker::tracked_objects_completed() const
{
  return registry.empty();
}

void BackfillState::ProgressTracker::enqueue_push(const hobject_t& obj)
{
  ceph_assert(registry.find(obj) == std::end(registry));
  registry[obj] = op_source_t::enqueued_push;
}

void BackfillState::ProgressTracker::enqueue_drop(const hobject_t& obj)
{
  ceph_assert(registry.find(obj) == std::end(registry));
  registry[obj] = op_source_t::enqueued_drop;
}

void BackfillState::ProgressTracker::complete_to(
  const hobject_t& obj,
  const pg_stat_t&)
{
  if (auto completion_boundary = registry.find(obj);
      completion_boundary != std::end(registry)) {
    for (auto it = std::begin(registry);
         it->first < completion_boundary->first;
         it = registry.erase(it)) {
      const auto& [ key, state ] = *it;
      ceph_assert(state == op_source_t::enqueued_drop);
      ps().update_complete_backfill_object_stats(
        key,
        pg_stat_t{}); // add empty stat
    }
    registry.erase(completion_boundary);
  } else {
    ceph_assert("completing untracked object shall not happen" == nullptr);
  }
  if (Enqueuing::all_enqueued(ps(),
                              bs().backfill_info,
                              bs().peer_backfill_info) &&
      tracked_objects_completed()) {
    bs().last_backfill_started = hobject_t::get_max();
    ls().update_peers_last_backfill(hobject_t::get_max());
  } else {
    ls().update_peers_last_backfill(obj);
  }
}

} // namespace crimson::osd
