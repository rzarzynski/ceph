// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <iostream>
#include <seastar/core/future.hh>

#include "crimson/osd/osdmap_gate.h"
#include "crimson/osd/osd_operation.h"
#include "crimson/osd/osd_operations/common/pg_pipeline.h"
#include "crimson/osd/pg.h"
#include "crimson/osd/pg_activation_blocker.h"
#include "osd/osd_types.h"
#include "osd/PGPeeringEvent.h"
#include "osd/PeeringState.h"

namespace ceph {
  class Formatter;
}

namespace crimson::osd {

class OSD;
class ShardServices;
class PG;

class SnapTrimEvent final : public PhasedOperationT<SnapTrimEvent> {
public:
  static constexpr OperationTypeCode type = OperationTypeCode::snaptrim_event;

  SnapTrimEvent(Ref<PG> pg, snapid_t snapid)
    : pg(std::move(pg)),
      snapid(snapid) {}

  void print(std::ostream &) const final;
  void dump_detail(ceph::Formatter* f) const final;
  seastar::future<> start();
  seastar::future<> with_pg(
    ShardServices &shard_services, Ref<PG> pg);

private:
  CommonPGPipeline& pp();

  // bases on 998cb8c141bb89aafae298a9d5e130fbd78fe5f2
  struct SubOpBlocker : crimson::BlockerT<SubOpBlocker> {
    static constexpr const char* type_name = "CompoundOpBlocker";

    using id_done_t = std::pair<crimson::Operation::id_t, seastar::future<>>;

    void dump_detail(Formatter *f) const final;

    template <class... Args>
    void emplace_back(Args&&... args);

    seastar::future<> wait_completion();
  private:
    std::vector<id_done_t> subops;
  } subop_blocker;
  PipelineHandle handle;
  Ref<PG> pg;
  const snapid_t snapid;

public:
  PipelineHandle& get_handle() { return handle; }

  std::tuple<
    StartEvent,
    CommonPGPipeline::WaitForActive::BlockingEvent,
    PGActivationBlocker::BlockingEvent,
    CommonPGPipeline::RecoverMissing::BlockingEvent,
    CommonPGPipeline::GetOBC::BlockingEvent,
    CommonPGPipeline::Process::BlockingEvent,
    CompletionEvent
  > tracking_events;
};

} // namespace crimson::osd
