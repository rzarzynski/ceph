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

#include "include/assert.h"
#include "lockdep.h"
#include "common/ceph_context.h"

#include <pthread.h>

using namespace ceph;

class PerfCounters;

enum {
  l_mutex_first = 999082,
  l_mutex_wait,
  l_mutex_last
};

class Mutex {
private:
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
  void operator=(const Mutex &M);
  Mutex(const Mutex &M);

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
  Mutex(const std::string &n, recursive_finder_t = recursive_finder_t(), bool r = false, bool ld=true, bool bt=false,
	CephContext *cct = 0);
  ~Mutex();
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

  void Lock(bool no_lockdep=false);

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
  void Unlock();

  friend class Cond;


public:
  class Locker {
    Mutex &mutex;

  public:
    explicit Locker(Mutex& m) : mutex(m) {
      mutex.Lock();
    }
    ~Locker() {
      mutex.Unlock();
    }
  };
};


#endif
