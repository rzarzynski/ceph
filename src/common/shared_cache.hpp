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

#ifndef CEPH_SHAREDCACHE_H
#define CEPH_SHAREDCACHE_H

#include <map>
#include <list>
#include <functional>
#ifdef WITH_SEASTAR
#include <boost/smart_ptr/local_shared_ptr.hpp>
#include <boost/smart_ptr/make_local_shared.hpp>
#else
#include <memory>
#endif
#include "common/ceph_mutex.h"
#include "common/dout.h"
#include "include/unordered_map.h"

// re-include our assert to clobber the system one; fix dout:
#include "include/ceph_assert.h"

template <class K, class V>
class SharedLRU {
  CephContext *cct;
#ifdef WITH_SEASTAR
  using VPtr = boost::local_shared_ptr<V>;
  using WeakVPtr = boost::weak_ptr<V>;
#else
  using VPtr = std::shared_ptr<V>;
  using WeakVPtr = std::weak_ptr<V>;
#endif

  struct weak_ref_t {
    WeakVPtr ptr;
    bool invalid = false;
  };
  typedef std::map<K, weak_ref_t, std::less<K>> weak_refs_t;

  class Cleanup {
    SharedLRU<K, V>* cache;
    typename weak_refs_t::iterator iter;
    static_assert(std::is_trivially_destructible_v<decltype(cache)>);
    static_assert(std::is_trivially_destructible_v<decltype(iter)>);

  public:
    Cleanup(SharedLRU<K, V>* const cache,
	    typename weak_refs_t::iterator iter)
      : cache(cache),
	iter(iter) {
    }
    void operator()(V* const ptr) {
      cache->remove(iter);
      delete ptr;
    }
  };

  // https://stackoverflow.com/questions/32113594/weak-ptr-make-shared-and-memory-deallocation
  template <class T>
  class VCleaningAlloc {
  public:
    SharedLRU<K, V>* cache;
    typename weak_refs_t::iterator iter;
    static_assert(std::is_trivially_destructible_v<decltype(cache)>);
    static_assert(std::is_trivially_destructible_v<decltype(iter)>);

    typedef T value_type;

    VCleaningAlloc(SharedLRU<K, V>* const cache,
		   typename weak_refs_t::iterator iter)
      : cache(cache),
	iter(iter) {
    }

    template<typename U>
    VCleaningAlloc(const VCleaningAlloc<U>& other)
      : cache(other.cache),
	iter(other.iter) {
    }

    T* allocate(const std::size_t n) {
      if (auto ptr = std::malloc(n * sizeof(T))) {
        return static_cast<T*>(ptr);
      }
      throw std::bad_alloc();
    }

    void destroy(T* ptr) {
      cache->remove(iter);
    }
    void deallocate(T* ptr, std::size_t n) {
      std::free(ptr);
    }

    bool operator==(const VCleaningAlloc<T>& rhs) const {
      return std::addressof(rhs) == this;
    }
    bool operator!=(const VCleaningAlloc<T>& rhs) const {
      return std::addressof(rhs) != this;
    }
  };

  ceph::mutex lock;
  size_t max_size;
  ceph::condition_variable cond;
  unsigned size;
#ifndef WITH_SEASTAR
public:
  int waiting;
#endif
private:
  using H = std::hash<K>;
  ceph::unordered_map<
    std::reference_wrapper<const K>,
    typename std::list<std::pair<K, VPtr> >::iterator, H> contents;
  std::list<std::pair<K, VPtr> > lru;

  // it seems that PrimaryLogPG::object_contexts doesn't really have
  // strict ordering requirements. Useful info as find() on std::map
  // with hobjects is costly.
  weak_refs_t weak_refs;

  void trim_cache(std::list<VPtr> *to_release) {
    while (size > max_size) {
      to_release->push_back(lru.back().second);
      lru_remove(lru.back().first);
    }
  }

  VPtr lru_lookup(const K& key) {
    if (const auto i = contents.find(key); i != std::end(contents)) {
      lru.splice(std::begin(lru), lru, i->second);
      return lru.front().second;
    }
    return VPtr();
  }

  void lru_remove(const K& key) {
    if (const auto i = contents.find(key); i != contents.end()) {
      lru.erase(i->second);
      --size;
      contents.erase(i);
    }
  }

  void lru_add(const K& key, const VPtr& val, std::list<VPtr> *to_release) {
    const auto i = contents.find(key);
    if (i != contents.end()) {
      lru.splice(lru.begin(), lru, i->second);
    } else {
      ++size;
      lru.push_front(std::make_pair(key, val));
      contents[std::cref(lru.front().first)] = lru.begin();
      trim_cache(to_release);
    }
  }

  void remove(typename weak_refs_t::iterator i) {
    std::lock_guard l{lock};
    if (!weak_refs.empty() && i != std::end(weak_refs)) {
      weak_refs.erase(i);
    }
    cond.notify_all();
  }

public:
  SharedLRU(CephContext *cct = NULL, size_t max_size = 20)
    : cct(cct),
      lock{ceph::make_mutex("SharedLRU::lock")},
      max_size(max_size),
#ifndef WITH_SEASTAR
      size(0), waiting(0) {
#else
      size(0) {
#endif
    contents.rehash(max_size); 
  }
  
  ~SharedLRU() {
    contents.clear();
    lru.clear();
    if (!weak_refs.empty()) {
      lderr(cct) << "leaked refs:\n";
      dump_weak_refs(*_dout);
      *_dout << dendl;
#ifndef WITH_SEASTAR
      if (cct->_conf.get_val<bool>("debug_asserts_on_shutdown")) {
#else
      if (ceph::common::local_conf().get_val<bool>("debug_assserts_on_shutdown")) {
#endif
	ceph_assert(weak_refs.empty());
      }
    }
  }

  int get_count() {
    std::lock_guard locker{lock};
    return size;
  }

  void set_cct(CephContext *c) {
    cct = c;
  }

  void dump_weak_refs() {
    lderr(cct) << "leaked refs:\n";
    dump_weak_refs(*_dout);
    *_dout << dendl;
  }

  void dump_weak_refs(std::ostream& out) {
    for (const auto& [key, ref] : weak_refs) {
      out << __func__ << " " << this << " weak_refs: "
	  << key << " with " << ref.ptr.use_count() << " refs"
	  << std::endl;
    }
  }

  //clear all strong reference from the lru.
  void clear() {
    while (true) {
#ifndef WITH_SEASTAR
      VPtr val; // release any ref we have after we drop the lock
#endif
      std::lock_guard locker{lock};
      if (size == 0)
        break;
#ifndef WITH_SEASTAR
      val = lru.back().second;
#endif
      lru_remove(lru.back().first);
    }
  }

  void clear(const K& key) {
#ifndef WITH_SEASTAR
    VPtr val; // release any ref we have after we drop the lock
    {
      std::lock_guard l{lock};
      if (auto i = weak_refs.find(key); i != std::end(weak_refs)) {
	val = i->second.ptr.lock();
      }
      lru_remove(key);
    }
#else
    lru_remove(key);
#endif
  }

  void purge(const K &key) {
#ifndef WITH_SEASTAR
    VPtr val; // release any ref we have after we drop the lock
#endif
    {
      std::lock_guard l{lock};
      if (auto i = weak_refs.find(key); i != std::end(weak_refs)) {
#ifndef WITH_SEASTAR
	val = i->second.ptr.lock();
#endif
	// TODO, FIXME: implement ALL invalidation checks
        i->second.invalid = true;
      }
      lru_remove(key);
    }
  }

  void set_size(size_t new_size) {
    list<VPtr> to_release;
    {
      std::lock_guard l{lock};
      max_size = new_size;
      trim_cache(&to_release);
    }
  }

  // Returns K key s.t. key <= k for all currently cached k,v
  K cached_key_lower_bound() {
    std::lock_guard l{lock};
    return weak_refs.begin()->first;
  }

  VPtr lower_bound(const K& key) {
    VPtr val;
    list<VPtr> to_release;
    {
      std::unique_lock l{lock};
#ifndef WITH_SEASTAR
      ++waiting;
#endif
      cond.wait(l, [this, &key, &val, &to_release] {
        if (weak_refs.empty()) {
          return true;
        }
        auto i = weak_refs.lower_bound(key);
        if (i == weak_refs.end()) {
          --i;
        }
        if (val = i->second.ptr.lock(); val) {
          lru_add(i->first, val, &to_release);
          return true;
        } else {
          return false;
        }
      });
#ifndef WITH_SEASTAR
      --waiting;
#endif
    }
    return val;
  }
  bool get_next(const K &key, std::pair<K, VPtr> *next) {
    std::pair<K, VPtr> r;
    {
      std::lock_guard l{lock};
      VPtr next_val;
      typename weak_refs_t::iterator i = weak_refs.upper_bound(key);

      while (i != weak_refs.end() &&
	     !(next_val = i->second.ptr.lock()))
	++i;

      if (i == weak_refs.end())
	return false;

      if (next)
	r = make_pair(i->first, next_val);
    }
    if (next)
      *next = r;
    return true;
  }
  bool get_next(const K &key, std::pair<K, V> *next) {
    std::pair<K, VPtr> r;
    bool found = get_next(key, &r);
    if (!found || !next)
      return found;
    next->first = r.first;
    ceph_assert(r.second);
    next->second = *(r.second);
    return found;
  }

  VPtr lookup(const K& key) {
    VPtr val;
    std::list<VPtr> to_release;
    {
      std::unique_lock l{lock};

      val = lru_lookup(key);
      if (val) {
	return val;
      }
#ifndef WITH_SEASTAR
      ++waiting;
#endif
      cond.wait(l, [this, &key, &val, &to_release] {
        if (auto i = weak_refs.find(key); i != std::end(weak_refs)) {
	  if (unlikely(i->second.invalid)) {
	    return true;
	  } else if (val = i->second.ptr.lock(); val) {
            lru_add(key, val, &to_release);
            return true;
          } else {
            return false;
          }
        } else {
          return true;
        }
      });
#ifndef WITH_SEASTAR
      --waiting;
#endif
    }
    return val;
  }
  VPtr lookup_or_create(const K &key) {
    VPtr val;
    std::list<VPtr> to_release;
    {
      std::unique_lock l{lock};

      val = lru_lookup(key);
      if (val) {
	return val;
      }
      cond.wait(l, [this, &key, &val] {
        if (auto i = weak_refs.find(key); i != std::end(weak_refs)) {
	  if (unlikely(i->second.invalid)) {
	    return true;
	  } else if (val = i->second.ptr.lock(); val) {
            return true;
          } else {
            return false;
          }
        } else {
          return true;
        }
      });
      if (!val) {
	auto [ iter, inserted ] = \
	  weak_refs.insert(std::make_pair(key, weak_ref_t()));
	if (likely(inserted)) {
#ifdef WITH_SEASTAR
	  val = boost::allocate_local_shared<V>(VCleaningAlloc<V>{this, iter});
#else
	  val = std::allocate_shared<V>(VCleaningAlloc<V>{this, iter});
#endif
	  iter->second.ptr = val;
	} else {
#ifdef WITH_SEASTAR
	  val = boost::allocate_local_shared<V>(VCleaningAlloc<V>{this, std::end(weak_refs)});
#else
	  val = std::allocate_shared<V>(VCleaningAlloc<V>{this, std::end(weak_refs)});
#endif
	}
      }
      lru_add(key, val, &to_release);
    }
    return val;
  }

  /**
   * empty()
   *
   * Returns true iff there are no live references left to anything that has been
   * in the cache.
   */
  bool empty() {
    std::lock_guard l{lock};
    return weak_refs.empty();
  }

  /***
   * Inserts a key if not present, or bumps it to the front of the LRU if
   * it is, and then gives you a reference to the value. If the key already
   * existed, you are responsible for deleting the new value you tried to
   * insert.
   *
   * @param key The key to insert
   * @param value The value that goes with the key
   * @param existed Set to true if the value was already in the
   * map, false otherwise
   * @return A reference to the map's value for the given key
   */
  VPtr add(const K& key, V *value, bool *existed = NULL) {
    VPtr val;
    list<VPtr> to_release;
    {
      typename weak_refs_t::iterator actual;
      std::unique_lock l{lock};
      cond.wait(l, [this, &key, &actual, &val] {
	  actual = weak_refs.lower_bound(key);
	  if (actual != weak_refs.end() && actual->first == key && !actual->second.invalid) {
	    val = actual->second.ptr.lock();
	    if (val) {
	      return true;
	    } else {
	      return false;
	    }
	  } else {
	    return true;
	  }
      });

      if (val) {
	if (existed) {
	  *existed = true;
	}
	return val;
      }

      if (existed)      
        *existed = false;

      auto [ iter, inserted ] = \
	weak_refs.insert(std::make_pair(key, weak_ref_t()));
      if (likely(inserted)) {
	val = VPtr(value, Cleanup(this, iter));
	iter->second.ptr = val;
      } else {
	// FIXME: no need for cleaning weak_refs up
	val = VPtr(value, Cleanup(this, std::end(weak_refs)));
      }
      lru_add(key, val, &to_release);
    }
    return val;
  }

  friend class SharedLRUTest;
};

#endif
