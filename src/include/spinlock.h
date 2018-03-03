// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 SUSE LINUX GmbH
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 * @author Jesse Williamson <jwilliamson@suse.de>
 *
*/

#ifndef CEPH_SPINLOCK_HPP
#define CEPH_SPINLOCK_HPP

#include <atomic>
#include <thread>

#include <immintrin.h>

#include "common/likely.h"

namespace ceph {
inline namespace version_1_0 {

class spinlock;

inline void spin_lock(std::atomic_bool& locked);
inline void spin_unlock(std::atomic_bool& locked);
inline void spin_lock(ceph::spinlock& locked);
inline void spin_unlock(ceph::spinlock& locked);

/* A pre-packaged spinlock type modelling BasicLockable: */
class spinlock final
{
  // Not using atomic_flag anymore because it doesn't
  // provide the load nor store operation.
  std::atomic_bool locked = false;

  // In contrast to atomic_flag, atomic_bool might be
  // implemented on top of e.g. mutex. However, it is
  // very unlikely we'll face such situation in Ceph.
  static_assert(std::atomic_bool::is_always_lock_free);

public:
  void lock() {
    ceph::spin_lock(locked);
  }
 
  void unlock() noexcept {
    ceph::spin_unlock(locked);
  }
};

// Free functions:
inline void spin_lock(std::atomic_bool& locked)
{
  bool expected = false;
  if (likely(locked.compare_exchange_weak(expected, true,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed))) {
    return;
  }

  do {
    std::size_t r3 = 0;
    do {
      asm("pause");
      if (++r3 == 32) {
        std::this_thread::yield();
      }
    } while (locked.load(std::memory_order_relaxed));

    expected = false;
  } while (!locked.compare_exchange_weak(expected, true,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed));
}

inline void spin_unlock(std::atomic_bool& locked)
{
  locked.store(false, std::memory_order_release);
}

inline void spin_lock(std::atomic_bool *locked)
{
 spin_lock(*locked);
}

inline void spin_unlock(std::atomic_bool *locked)
{
 spin_unlock(*locked);
}

inline void spin_lock(ceph::spinlock& lock)
{
 lock.lock();
}

inline void spin_unlock(ceph::spinlock& lock)
{
 lock.unlock();
}

inline void spin_lock(ceph::spinlock *lock)
{
 spin_lock(*lock);
}

inline void spin_unlock(ceph::spinlock *lock)
{
 spin_unlock(*lock);
}

} // inline namespace (version)
} // namespace ceph

#endif
