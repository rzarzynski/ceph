// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include "crimson/osd/backfill_state.h"
#include "crimson/osd/pg.h"
#include "osd/PeeringState.h"

namespace crimson::osd {

struct BackfillState::PeeringFacade {
  PeeringState& peering_state;

  decltype(auto) earliest_backfill() const {
    return peering_state.earliest_backfill();
  }

  decltype(auto) get_backfill_targets() const {
    return peering_state.get_backfill_targets();
  }

  decltype(auto) get_peer_info(pg_shard_t peer) const {
    return peering_state.get_peer_info(peer);
  }

  decltype(auto) get_info() const {
    return peering_state.get_info();
  }

  decltype(auto) get_pg_log() const {
    return peering_state.get_pg_log();
  }
  void update_complete_backfill_object_stats(const hobject_t &hoid,
                                             const pg_stat_t &stats) {
    return peering_state.update_complete_backfill_object_stats(hoid, stats);
  }

  PeeringFacade(PeeringState& peering_state)
    : peering_state(peering_state) {
  }
};

struct BackfillState::PGFacade {
  PG& pg;

  decltype(auto) get_projected_last_update() const {
    return pg.projected_last_update;
  }

  PGFacade(PG& pg) : pg(pg) {}
};

} // namespace crimson::osd
