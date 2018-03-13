// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/Mutex.h"
#include "common/perf_counters.h"
#include "common/config.h"
#include "common/Clock.h"
#include "common/valgrind.h"

namespace ceph::mutex_helpers {

enum {
  l_mutex_first = 999082,
  l_mutex_wait,
  l_mutex_last
};


PerfCounters* build_perf_counters(CephContext* const cct,
                                  const std::string& name)
{
  PerfCountersBuilder b(cct, string("mutex-") + name,
                        l_mutex_first, l_mutex_last);
  b.add_time_avg(l_mutex_wait, "wait",
                 "Average time of mutex in locked state");
  PerfCounters* const logger = b.create_perf_counters();
  cct->get_perfcounters_collection()->add(logger);
  logger->set(l_mutex_wait, 0);
  return logger;
}

void dispose_perf_counters(CephContext* const cct,
                           PerfCounters** logger)
{
  if (cct && *logger) {
    cct->get_perfcounters_collection()->remove(*logger);
    delete *logger;
    *logger = nullptr;
  }
}

} // namespace ceph::mutex_helpers

