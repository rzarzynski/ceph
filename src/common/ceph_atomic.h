// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

// What and why
// ============
//
// ceph::atomic – thin wrapper to differentiate behavior of atomics.
//
// Not all users of the common truly need costly atomic operations to
// synchronize data between CPUs and threads. Some, like crimson-osd,
// stick to shared-nothing approach. Enforcing issue of atomics in
// such cases is wasteful – on x86 any locked instruction works actually
// like a full memory barrier stalling execution till CPU's store and
// load buffers are drained.

#ifdef WITH_SEASTAR

namespace ceph {
  template <class T> using atomic = T;
} // namespace ceph

#else  // WITH_SEASTAR
#  include <atomic>

namespace ceph {
  template <class T> using atomic = ::std::atomic<T>;
} // namespace ceph

#endif	// WITH_SEASTAR
