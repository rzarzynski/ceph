// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <map>

#include "crimson/common/operation.h"
#include "crimson/osd/scheduler/scheduler.h"
#include "osd/osd_types.h"

namespace crimson::osd {

enum class OperationTypeCode {
  client_request = 0,
  peering_event,
  compound_peering_request,
  pg_advance_map,
  pg_creation,
  replicated_request,
  background_recovery,
  background_recovery_sub,
  last_op
};

static constexpr const char* const OP_NAMES[] = {
  "client_request",
  "peering_event",
  "compound_peering_request",
  "pg_advance_map",
  "pg_creation",
  "replicated_request",
  "background_recovery",
  "background_recovery_sub",
};

// prevent the addition of OperationTypeCode-s with no matching OP_NAMES entry:
static_assert(
  (sizeof(OP_NAMES)/sizeof(OP_NAMES[0])) ==
  static_cast<int>(OperationTypeCode::last_op));

template <typename>
struct OperationComparator;

template <typename T>
class OperationRepeatSequencer {
public:
  using OpRef = boost::intrusive_ptr<T>;
  using ops_sequence_t = std::map<OpRef, seastar::promise<>>;

  template <typename Func, typename HandleT, typename Result = std::invoke_result_t<Func>>
  seastar::futurize_t<Result> preserve_sequence(
      HandleT& handle,
      epoch_t same_interval_since,
      OpRef& op,
      const spg_t& pgid,
      Func&& func) {
    auto& ops = pg_ops[pgid];
    if (!op->pos) {
      auto [it, inserted] = ops.emplace(op, seastar::promise<>());
      assert(__builtin_expect(inserted, true));
      op->pos = it;
    }

    bool first = (*(op->pos) == ops.begin()) ? true : false;

    this->same_interval_since = same_interval_since;

    typename OperationRepeatSequencer<T>::ops_sequence_t::iterator last_op_it = *(op->pos);
    if (!first) {
      --last_op_it;
    }
    epoch_t last_op_interval_start =
      last_op_it->first->interval_start_epoch;

    if (first || last_op_interval_start == same_interval_since) {
      op->interval_start_epoch = same_interval_since;
      auto fut = seastar::futurize_invoke(func);
      auto it = *(op->pos);

      it->second.set_value();
      it->second = seastar::promise<>();
      return fut;
    } else {
      handle.exit();	// need to wait for previous operations,
			// release the current pipepline stage

      ::crimson::get_logger(ceph_subsys_osd).debug(
	  "{}, same_interval_since: {}, previous op: {}, last_interval_start: {}",
	  *op, same_interval_since, *(last_op_it->first), last_op_interval_start);
      assert(__builtin_expect(last_op_interval_start < same_interval_since, true));
      return last_op_it->second.get_future().then(
	[op, same_interval_since, func=std::forward<Func>(func)]() mutable {
	op->interval_start_epoch = same_interval_since;
	auto fut = seastar::futurize_invoke(std::forward<Func>(func));
	auto it = *(op->pos);
	it->second.set_value();
	it->second = seastar::promise<>();
	return fut;
      });
    }
  }

  void operation_finished(OpRef& op, const spg_t& pgid, bool error = false) {
    if (op->pos) {
      if (error) {
	(*(op->pos))->second.set_value();
      }
      pg_ops.at(pgid).erase(*(op->pos));
    }
  }
private:
  std::map<spg_t, std::map<OpRef, seastar::promise<>, OperationComparator<T>>> pg_ops;
  epoch_t same_interval_since = 0;
};
template <typename T>
struct OperationComparator {
  bool operator()(
    const typename OperationRepeatSequencer<T>::OpRef& left,
    const typename OperationRepeatSequencer<T>::OpRef& right) const;
};

template <typename T>
bool OperationComparator<T>::operator()(
  const typename OperationRepeatSequencer<T>::OpRef& left,
  const typename OperationRepeatSequencer<T>::OpRef& right) const {
  return left->get_id() < right->get_id();
}

template <typename T>
class OperationT : public Operation {
public:
  static constexpr const char *type_name = OP_NAMES[static_cast<int>(T::type)];
  using IRef = boost::intrusive_ptr<T>;

  unsigned get_type() const final {
    return static_cast<unsigned>(T::type);
  }

  const char *get_type_name() const final {
    return T::type_name;
  }

  virtual ~OperationT() = default;

private:
  epoch_t interval_start_epoch = 0;
  std::optional<typename OperationRepeatSequencer<T>::ops_sequence_t::iterator> pos;
  virtual void dump_detail(ceph::Formatter *f) const = 0;
  template <typename>
  friend class OperationRepeatSequencer;
};

/**
 * Maintains a set of lists of all active ops.
 */
using OSDOperationRegistry = OperationRegistryT<
  static_cast<size_t>(OperationTypeCode::last_op)
  >;

/**
 * Throttles set of currently running operations
 *
 * Very primitive currently, assumes all ops are equally
 * expensive and simply limits the number that can be
 * concurrently active.
 */
class OperationThrottler : public Blocker,
			private md_config_obs_t {
public:
  OperationThrottler(ConfigProxy &conf);

  const char** get_tracked_conf_keys() const final;
  void handle_conf_change(const ConfigProxy& conf,
			  const std::set<std::string> &changed) final;
  void update_from_config(const ConfigProxy &conf);

  template <typename F>
  auto with_throttle(
    OperationRef op,
    crimson::osd::scheduler::params_t params,
    F &&f) {
    if (!max_in_progress) return f();
    auto fut = acquire_throttle(params);
    return op->with_blocking_future(std::move(fut))
      .then(std::forward<F>(f))
      .then([this](auto x) {
	release_throttle();
	return x;
      });
  }

  template <typename F>
  seastar::future<> with_throttle_while(
    OperationRef op,
    crimson::osd::scheduler::params_t params,
    F &&f) {
    return with_throttle(op, params, f).then([this, params, op, f](bool cont) {
      if (cont)
	return with_throttle_while(op, params, f);
      else
	return seastar::make_ready_future<>();
    });
  }

private:
  void dump_detail(Formatter *f) const final;
  const char *get_type_name() const final {
    return "OperationThrottler";
  }

private:
  crimson::osd::scheduler::SchedulerRef scheduler;

  uint64_t max_in_progress = 0;
  uint64_t in_progress = 0;

  uint64_t pending = 0;

  void wake();

  blocking_future<> acquire_throttle(
    crimson::osd::scheduler::params_t params);

  void release_throttle();
};

}
