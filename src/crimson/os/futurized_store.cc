// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "futurized_store.h"
#include "cyanstore/cyan_store.h"
#ifdef WITH_BLUESTORE
#include "alienstore/alien_store.h"
#endif
#include "relaystore/relay_store.h"
#include "seastore/seastore.h"

namespace crimson::os {

std::unique_ptr<FuturizedStore>
FuturizedStore::create(const std::string& type,
                       const std::string& data,
                       const ConfigValues& values)
{
  const bool same_no_shards =
    crimson::common::get_conf<bool>("seastore_require_partition_count_match_reactor_count");
  std::unique_ptr<FuturizedStore> store;
  if (type == "cyanstore") {
    store = std::make_unique<CyanStore>(data);
  } else if (type == "seastore") {
    store = crimson::os::seastore::make_seastore(data);
  } else {
#ifdef WITH_BLUESTORE
    // use AlienStore as a fallback. It adapts e.g. BlueStore.
    return std::make_unique<AlienStore>(type, data, values);
#else
    ceph_abort_msgf("unsupported objectstore type: %s", type.c_str());
    return {};
#endif
  }
  if (same_no_shards) {
    return store;
  } else {
    return std::make_unique<_RelayStore>(std::move(store));
  }
}

}
