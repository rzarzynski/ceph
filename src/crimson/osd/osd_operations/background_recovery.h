// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <boost/statechart/event_base.hpp>

#include "crimson/net/Connection.h"
#include "crimson/osd/osd_operation.h"
#include "crimson/common/type_helpers.h"

class MOSDOp;

namespace crimson::osd {
class PG;
class ShardServices;

class BackgroundRecovery : public OperationT<BackgroundRecovery> {
public:
  static constexpr OperationTypeCode type = OperationTypeCode::background_recovery;

  BackgroundRecovery(
    Ref<PG> pg,
    ShardServices &ss,
    epoch_t epoch_started,
    crimson::osd::scheduler::scheduler_class_t scheduler_class);

  void print(std::ostream &) const final;
  void dump_detail(Formatter *f) const final;
  seastar::future<> start();
protected:
  Ref<PG> pg;
  ShardServices &ss;
  epoch_t epoch_started;
  crimson::osd::scheduler::scheduler_class_t scheduler_class;

  auto get_scheduler_params() const {
    return crimson::osd::scheduler::params_t{
      1, // cost
      0, // owner
      scheduler_class
    };
  }

  virtual seastar::future<bool> do_recovery() = 0;
};

class PglogBasedRecovery final : public BackgroundRecovery {
  seastar::future<bool> do_recovery() override;

public:
  PglogBasedRecovery(
    Ref<PG> pg,
    ShardServices &ss,
    epoch_t epoch_started);
};

class BackfillRecovery final : public BackgroundRecovery {
  boost::intrusive_ptr<const boost::statechart::event_base> evt;
  OrderedPipelinePhase::Handle handle;
  seastar::future<bool> do_recovery() override;

public:
  class BackfillRecoveryPipeline {
    OrderedPipelinePhase process = {
      "BackfillRecovery::PGPipeline::process"
    };
    friend class BackfillRecovery;
    friend class PeeringEvent;
  };

  template <class EventT>
  BackfillRecovery(
    Ref<PG> pg,
    ShardServices &ss,
    epoch_t epoch_started,
    const EventT& evt);

  static BackfillRecoveryPipeline &bp(PG &pg);
};

template <class EventT>
BackfillRecovery::BackfillRecovery(
  Ref<PG> pg,
  ShardServices &ss,
  const epoch_t epoch_started,
  const EventT& evt)
  : BackgroundRecovery(
      std::move(pg),
      ss,
      epoch_started,
      crimson::osd::scheduler::scheduler_class_t::background_best_effort),
    evt(evt.intrusive_from_this())
{}


}
