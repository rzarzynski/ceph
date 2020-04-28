// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <boost/statechart/custom_reaction.hpp>
#include <boost/statechart/event.hpp>
#include <boost/statechart/simple_state.hpp>
#include <boost/statechart/state.hpp>
#include <boost/statechart/state_machine.hpp>
#include <boost/statechart/transition.hpp>
#include <boost/statechart/event_base.hpp>

#include "osd/PeeringState.h"

namespace crimson::osd {

namespace sc = boost::statechart;

struct BackfillState {
  struct BackfillListener;
  struct PeeringFacade;
  struct PGFacade;

  // events comes first
  struct PrimaryScanned : sc::event<PrimaryScanned> {
    BackfillInterval result;
  };

  struct ReplicaScanned : sc::event<ReplicaScanned> {
    pg_shard_t from;
    BackfillInterval result;
  };

  struct ObjectPushed : sc::event<ObjectPushed> {
    // TODO: implement replica management; I don't want to follow
    // current convention where the backend layer is responsible
    // for tracking replicas.
    pg_shard_t replica; // on which replica
    hobject_t object;
    pg_stat_t stat;
  };

  struct Trigerred : sc::event<Trigerred> {
  };

private:
  // internal events
  struct RequestPrimaryScanning : sc::event<RequestPrimaryScanning> {
  };

  struct RequestReplicasScanning : sc::event<RequestReplicasScanning> {
  };

  struct RequestWaiting : sc::event<RequestWaiting> {
  };

  struct RequestDone : sc::event<RequestDone> {
  };

public:

  struct Initial;
  struct Enqueuing;
  struct PrimaryScanning;
  struct ReplicasScanning;
  struct Waiting;

  struct BackfillMachine : sc::state_machine<BackfillMachine, Initial> {
    BackfillState& backfill_state;
    BackfillListener& backfill_listener;
    PeeringFacade& peering_state;
    PGFacade& pg;
  };

private:
  template <class S>
  struct StateHelper {
    BackfillState& bs() {
      return static_cast<S*>(this) \
        ->template context<BackfillMachine>().backfill_state;
    }
    BackfillListener& ls() {
      return static_cast<S*>(this) \
        ->template context<BackfillMachine>().backfill_listener;
    }
    PeeringFacade& ps() {
      return static_cast<S*>(this) \
        ->template context<BackfillMachine>().peering_state;
    }
    PGFacade& pg() {
      return static_cast<S*>(this)->template context<BackfillMachine>().pg;
    }

    const PeeringFacade& ps() const {
      return static_cast<const S*>(this) \
        ->template context<BackfillMachine>().peering_state;
    }
    const BackfillState& bs() const {
      return static_cast<const S*>(this) \
        ->template context<BackfillMachine>().backfill_state;
    }
  };

public:

  // states
  struct Crashed : sc::simple_state<Crashed, BackfillMachine>,
                   StateHelper<Crashed> {
  };

  struct Initial : sc::simple_state<Initial, BackfillMachine>,
                   StateHelper<Initial> {
    using reactions = boost::mpl::list<
      sc::transition<Trigerred, Enqueuing>,
      sc::transition<sc::event_base, Crashed>>;
    explicit Initial();
    // initialize after triggering backfill by on_activate_complete().
    // transit to Enqueuing.
    sc::result react(const Trigerred&);
  };

  struct Enqueuing : sc::simple_state<Enqueuing, BackfillMachine>,
                     StateHelper<Enqueuing> {
    using reactions = boost::mpl::list<
      sc::transition<RequestPrimaryScanning, PrimaryScanning>,
      sc::transition<RequestReplicasScanning, ReplicasScanning>,
      sc::transition<RequestWaiting, Waiting>,
      sc::transition<sc::event_base, Crashed>>;
    explicit Enqueuing();

    static bool all_peer_enqueued(
      const PeeringFacade& ps,
      const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info);
    static bool all_enqueued(
      const PeeringFacade& ps,
      const BackfillInterval& backfill_info,
      const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info);

  private:
    void maybe_update_range();
    void trim_backfill_infos();

    // these methods take BackfillIntervals instead of extracting them from
    // the state to emphasize the relationships across the main loop.
    bool all_peer_enqueued(
      const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info) const;
    hobject_t earliest_peer_backfill(
      const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info) const;
    bool should_rescan_replicas(
      const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info,
      const BackfillInterval& backfill_info) const;
    bool should_rescan_primary(
      const std::map<pg_shard_t, BackfillInterval>& peer_backfill_info,
      const BackfillInterval& backfill_info) const;

    struct loop_advancer_t {
      std::set<pg_shard_t> pbi_targets;
      hobject_t new_last_backfill_started;
      void advance(
        hobject_t& last_backfill_started,
        std::map<pg_shard_t, BackfillInterval>& peer_backfill_info) &&;
    };
    loop_advancer_t remove_on_peers(const hobject_t& check);
    loop_advancer_t update_on_peers(const hobject_t& check);
  };

  struct PrimaryScanning : sc::simple_state<PrimaryScanning, BackfillMachine>,
                           StateHelper<PrimaryScanning> {
    using reactions = boost::mpl::list<
      sc::custom_reaction<ObjectPushed>,
      sc::custom_reaction<PrimaryScanned>,
      sc::transition<sc::event_base, Crashed>>;
    explicit PrimaryScanning();
    sc::result react(ObjectPushed);
    // collect scanning result and transit to Enqueuing.
    sc::result react(PrimaryScanned);
  };

  struct ReplicasScanning : sc::simple_state<ReplicasScanning, BackfillMachine>,
                            StateHelper<ReplicasScanning> {
    using reactions = boost::mpl::list<
      sc::custom_reaction<ObjectPushed>,
      sc::custom_reaction<ReplicaScanned>,
      sc::transition<sc::event_base, Crashed>>;
    explicit ReplicasScanning();
    // collect scanning result; if all results are collected, transition
    // to Enqueuing will happen.
    sc::result react(ObjectPushed);
    sc::result react(ReplicaScanned);

    static bool replica_needs_scan(
      const BackfillInterval& replica_backfill_info,
      const BackfillInterval& local_backfill_info);
  };

  struct Waiting : sc::simple_state<Waiting, BackfillMachine>,
                   StateHelper<Waiting> {
    using reactions = boost::mpl::list<
      sc::custom_reaction<ObjectPushed>,
      sc::transition<sc::event_base, Crashed>>;
    explicit Waiting();
    sc::result react(ObjectPushed);
  };

  struct Done : sc::simple_state<Done, BackfillMachine>,
                StateHelper<Done> {
    using reactions = boost::mpl::list<
      sc::transition<sc::event_base, Crashed>>;
    explicit Done();
  };

private:
  hobject_t last_backfill_started;
  BackfillInterval backfill_info;
  std::map<pg_shard_t, BackfillInterval> peer_backfill_info;
  std::set<hobject_t> backfills_in_flight;
  std::map<hobject_t, pg_stat_t> pending_backfill_updates;
  std::set<pg_shard_t> waiting_on_backfill;
};

struct BackfillState::BackfillListener {
  virtual void request_replica_scan(
    const pg_shard_t& target,
    const hobject_t& begin,
    const hobject_t& end) = 0;

  virtual void request_primary_scan(
    const hobject_t& begin) = 0;

  virtual void enqueue_push(
    const pg_shard_t& target,
    const hobject_t& obj,
    const eversion_t& v) = 0;

  virtual void enqueue_drop(
    const pg_shard_t& target,
    const hobject_t& obj,
    const eversion_t& v) = 0;

  virtual void update_peers_last_backfill(
    const hobject_t& new_last_backfill) = 0;

  virtual bool budget_available() const = 0;

  virtual void backfilled() = 0;

  virtual ~BackfillListener() = default;
};

} // namespace crimson::osd
