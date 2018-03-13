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

#ifndef CEPH_MUTEX_H
#define CEPH_MUTEX_H

#include <pthread.h>

#include "include/assert.h"
#include "lockdep.h"
#include "common/Clock.h"
#include "common/ceph_context.h"
#include "common/valgrind.h"

using namespace ceph;

class PerfCounters;
class Cond;

namespace ceph {

// The PerfCounters-related stuff stays on the cold path, so there
// is no business in migrating it here, and thus acing the need to
// resolve the circular dependency we have between it and Mutex.
namespace mutex_helpers {

extern PerfCounters* build_perf_counters(
  CephContext* cct,
  const std::string& name);

extern void dispose_perf_counters(
  CephContext* const cct,
  PerfCounters** logger);
}

class mutex {
  std::string name;
  int id;
  bool recursive;
  bool lockdep;
  bool backtrace;  // gather backtrace on lock acquisition

  mutable pthread_mutex_t _m;
  pthread_t locked_by;
  CephContext *cct;
  PerfCounters *logger;

  // don't allow copying.
  void operator=(const mutex& M);
  mutex(const mutex& M);

  void _register() {
    id = lockdep_register(name.c_str());
  }
  void _will_lock() { // about to lock
    id = lockdep_will_lock(name.c_str(), id, backtrace);
  }
  void _locked() {    // just locked
    id = lockdep_locked(name.c_str(), id, backtrace);
  }
  void _will_unlock() {  // about to unlock
    id = lockdep_will_unlock(name.c_str(), id);
  }

public:
  struct recursive_finder_t {};
  mutex(const std::string &n, recursive_finder_t = recursive_finder_t(), bool r = false, bool ld=true, bool bt=false,
	CephContext *cct = 0) :
  name(n), id(-1), recursive(r), lockdep(ld), backtrace(bt),
  locked_by(0), cct(cct), logger(0)
  {
    ANNOTATE_BENIGN_RACE_SIZED(&id, sizeof(id), "Mutex lockdep id");
    ANNOTATE_BENIGN_RACE_SIZED(&locked_by, sizeof(locked_by), "Mutex locked_by");
    if (cct) {
      logger = mutex_helpers::build_perf_counters(cct, name);
    }
    if (recursive) {
      // Mutexes of type PTHREAD_MUTEX_RECURSIVE do all the same checks as
      // mutexes of type PTHREAD_MUTEX_ERRORCHECK.
      pthread_mutexattr_t attr;
      pthread_mutexattr_init(&attr);
      pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
      pthread_mutex_init(&_m,&attr);
      pthread_mutexattr_destroy(&attr);
      if (lockdep && g_lockdep)
        _register();
    }
    else if (lockdep) {
      // If the mutex type is PTHREAD_MUTEX_ERRORCHECK, then error checking
      // shall be provided. If a thread attempts to relock a mutex that it
      // has already locked, an error shall be returned. If a thread
      // attempts to unlock a mutex that it has not locked or a mutex which
      // is unlocked, an error shall be returned.
      pthread_mutexattr_t attr;
      pthread_mutexattr_init(&attr);
      pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
      pthread_mutex_init(&_m, &attr);
      pthread_mutexattr_destroy(&attr);
      if (g_lockdep)
        _register();
    }
    else {
      // If the mutex type is PTHREAD_MUTEX_DEFAULT, attempting to recursively
      // lock the mutex results in undefined behavior. Attempting to unlock the
      // mutex if it was not locked by the calling thread results in undefined
      // behavior. Attempting to unlock the mutex if it is not locked results in
      // undefined behavior.
      pthread_mutex_init(&_m, NULL);
    }
  }
  ~mutex() {
    // helgrind gets confused by condition wakeups leading to mutex destruction
    ANNOTATE_BENIGN_RACE_SIZED(&_m, sizeof(_m), "Mutex primitive");
    pthread_mutex_destroy(&_m);

    mutex_helpers::dispose_perf_counters(cct, &logger);

    if (lockdep && g_lockdep) {
      lockdep_unregister(id);
    }
  }

  bool is_locked() const {
    if (recursive) {
      // ugly hack for testing the whole concept. The recursive
      // Mutex will be dissected into separate class if succeeded.
      return true;
    }

    if (pthread_mutex_trylock(&_m) != 0) {
      return true;
    } else {
      pthread_mutex_unlock(&_m);
      return false;
    }
  }
  bool is_locked_by_me() const {
    return is_locked() && locked_by == pthread_self();
  }

  bool TryLock() {
    int r = pthread_mutex_trylock(&_m);
    if (r == 0) {
      if (lockdep && g_lockdep) _locked();
      _post_lock();
    }
    return r == 0;
  }

  void Lock(bool no_lockdep=false) {
    int r;

    if (lockdep && g_lockdep && !no_lockdep && !recursive) _will_lock();

#if 0
    if (logger && cct && cct->_conf->mutex_perf_counter) {
      utime_t start;
      // instrumented mutex enabled
      start = ceph_clock_now();
      if (TryLock()) {
        goto out;
      }

      r = pthread_mutex_lock(&_m);

      logger->tinc(l_mutex_wait,
		   ceph_clock_now() - start);
    } else {
      r = pthread_mutex_lock(&_m);
    }
#endif

    assert(r == 0);
    if (lockdep && g_lockdep) _locked();
    _post_lock();

#if 0
  out:
    ;
#endif
  }

  void _post_lock() {
    if (!recursive) {
      locked_by = pthread_self();
    };
  }

  void _pre_unlock() {
    if (!recursive) {
      assert(locked_by == pthread_self());
      locked_by = 0;
    }
  }
  void Unlock() {
    _pre_unlock();
    if (lockdep && g_lockdep) _will_unlock();
    int r = pthread_mutex_unlock(&_m);
    assert(r == 0);
  }

  friend Cond;


public:
  class Locker {
    mutex& m_mutex;

  public:
    explicit Locker(mutex& m) : m_mutex(m) {
      m_mutex.Lock();
    }
    ~Locker() {
      m_mutex.Unlock();
    }
  };
};

} // namespace ceph

// For backward compatibility. Typically a Mutex instance
// is being instantiated using the ctor's default parameters.
// We can leverage this fact and minimize the changes solely
// to those non-typical cases.
class Mutex : public ceph::mutex {
public:
  using ceph::mutex::mutex;
};

#endif
