// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "crimson/osd/object_context.h"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_osd);
  }
}

namespace crimson::osd {

seastar::future<> ObjectContext::get_lock_type(Operation *op, RWState::State type) {
  logger().info("{} for type={} on rwstate={}", __func__, type, rwstate);
  switch (type) {
  case RWState::RWWRITE:
    return get_lock(op, [this] { return rwstate.get_write_lock(); });
  case RWState::RWREAD:
    return get_lock(op, [this] { return rwstate.get_read_lock(); });
  case RWState::RWEXCL:
    return get_lock(op, [this] { return rwstate.get_excl_lock(); });
  case RWState::RWNONE:
    return seastar::now();
  default:
    ceph_abort_msg("invalid lock type");
    return seastar::now();
  }
}

void ObjectContext::dump_detail(Formatter *f) const
{
  f->open_object_section("ObjectContext");
  obs.oi.dump(f);
  f->close_section();
}

ObjectContextRegistry::ObjectContextRegistry(crimson::common::ConfigProxy &conf)
{
  obc_lru.set_target_size(conf.get_val<uint64_t>("crimson_osd_obc_lru_size"));
  conf.add_observer(this);
}

const char** ObjectContextRegistry::get_tracked_conf_keys() const
{
  static const char* KEYS[] = {
    "crimson_osd_obc_lru_size",
    nullptr
  };
  return KEYS;
}

void ObjectContextRegistry::handle_conf_change(
  const ConfigProxy& conf,
  const std::set <std::string> &changed)
{
  obc_lru.set_target_size(conf.get_val<uint64_t>("crimson_osd_obc_lru_size"));
}


}
