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

#ifndef CEPH_REFCOUNTEDOBJ_H
#define CEPH_REFCOUNTEDOBJ_H
 
#include "common/Mutex.h"
#include "common/Cond.h"
#include "common/ceph_context.h"
#include "common/valgrind.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

// re-include our assert to clobber the system one; fix dout:
#include "include/assert.h"

#undef REFCOUNTEDOBJECT_DEBUG

struct RefCountedObject {
private:
  mutable std::atomic<uint64_t> nref;
#ifdef REFCOUNTEDOBJECT_DEBUG
  CephContext *cct;
#endif
public:
  __attribute__((noinline)) RefCountedObject(CephContext *c = NULL, int n=1)
    : nref(n)
#ifdef REFCOUNTEDOBJECT_DEBUG
    , cct(c)
#endif
  {}
  virtual __attribute__((noinline)) ~RefCountedObject() {
    assert(nref == 0);
  }
  
  const RefCountedObject* __attribute__((noinline)) get() const {
#ifdef REFCOUNTEDOBJECT_DEBUG
    int v = ++nref;
    if (cct)
      lsubdout(cct, refs, 1) << "RefCountedObject::get " << this << " "
			     << (v - 1) << " -> " << v
			     << dendl;
#else
    ++nref;
#endif
    return this;
  }
  RefCountedObject* __attribute__((noinline)) get() {
#ifdef REFCOUNTEDOBJECT_DEBUG
    int v = ++nref;
    if (cct)
      lsubdout(cct, refs, 1) << "RefCountedObject::get " << this << " "
			     << (v - 1) << " -> " << v
			     << dendl;
#else
    ++nref;
#endif
    return this;
  }
  void __attribute__((noinline)) put() const {
    int v = --nref;
    if (v == 0) {
      delete this;
    }
#ifdef REFCOUNTEDOBJECT_DEBUG
    CephContext *local_cct = cct;
    if (local_cct)
      lsubdout(local_cct, refs, 1) << "RefCountedObject::put " << this << " "
				   << (v + 1) << " -> " << v
				   << dendl;
#endif
  }
  void __attribute__((noinline)) set_cct(CephContext *c) {
#ifdef REFCOUNTEDOBJECT_DEBUG
    cct = c;
#endif
  }

  uint64_t __attribute__((noinline)) get_nref() const {
    return nref;
  }
};

/**
 * RefCountedCond
 *
 *  a refcounted condition, will be removed when all references are dropped
 */

struct RefCountedCond : public RefCountedObject {
  bool complete;
  Mutex lock;
  Cond cond;
  int rval;

  RefCountedCond() : complete(false), lock("RefCountedCond"), rval(0) {}

  int wait() {
    Mutex::Locker l(lock);
    while (!complete) {
      cond.Wait(lock);
    }
    return rval;
  }

  void done(int r) {
    Mutex::Locker l(lock);
    rval = r;
    complete = true;
    cond.SignalAll();
  }

  void done() {
    done(0);
  }
};

/**
 * RefCountedWaitObject
 *
 * refcounted object that allows waiting for the object's last reference.
 * Any referrer can either put or put_wait(). A simple put() will return
 * immediately, a put_wait() will return only when the object is destroyed.
 * e.g., useful when we want to wait for a specific event completion. We
 * use RefCountedCond, as the condition can be referenced after the object
 * destruction. 
 *    
 */
struct RefCountedWaitObject {
  std::atomic<uint64_t> nref = { 1 };
  RefCountedCond *c;

  RefCountedWaitObject() {
    c = new RefCountedCond;
  }
  virtual ~RefCountedWaitObject() {
    c->put();
  }

  RefCountedWaitObject *get() {
    nref++;
    return this;
  }

  bool put() {
    bool ret = false;
    RefCountedCond *cond = c;
    cond->get();
    if (--nref == 0) {
      cond->done();
      delete this;
      ret = true;
    }
    cond->put();
    return ret;
  }

  void put_wait() {
    RefCountedCond *cond = c;

    cond->get();
    if (--nref == 0) {
      cond->done();
      delete this;
    } else {
      cond->wait();
    }
    cond->put();
  }
};

void intrusive_ptr_add_ref(const RefCountedObject *p);
void intrusive_ptr_release(const RefCountedObject *p);

using RefCountedPtr = boost::intrusive_ptr<RefCountedObject>;

#endif
