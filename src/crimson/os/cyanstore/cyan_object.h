// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <seastar/core/future.hh>

#include "include/buffer.h"

namespace crimson::os {

struct Object : public boost::intrusive_ref_counter<
  Object,
  boost::thread_unsafe_counter>
{
  class OmapIterator {
    std::map<std::string, bufferlist>::const_iterator iter;
    Object* obj;
  public:
    OmapIterator() {}
    OmapIterator(Object* obj) : obj(obj) {
      iter = obj->omap.begin();
    };
    virtual seastar::future<int> seek_to_first();
    virtual seastar::future<int> upper_bound(const std::string &after);
    virtual seastar::future<int> lower_bound(const std::string &to);
    virtual bool valid();
    virtual seastar::future<int> next();
    virtual std::string key() {
      return iter->first;
    }
    virtual seastar::future<std::string> tail_key() {
      return seastar::make_ready_future<std::string>((++obj->omap.end())->first);
    }
    virtual ceph::buffer::list value() {
      return iter->second;
    }
    virtual int status() {
      return iter != obj->omap.end() ? 0 : -1;
    }
    virtual ~OmapIterator() {}
  };
  using bufferlist = ceph::bufferlist;

  bufferlist data;
  // use transparent comparator for better performance, see
  // https://en.cppreference.com/w/cpp/utility/functional/less_void
  std::map<std::string,bufferptr,std::less<>> xattr;
  bufferlist omap_header;
  std::map<std::string,bufferlist> omap;

  typedef boost::intrusive_ptr<Object> Ref;

  Object() = default;

  // interface for object data
  size_t get_size() const;
  ceph::bufferlist read(uint64_t offset, uint64_t len);
  int write(uint64_t offset, const bufferlist &bl);
  int clone(Object *src, uint64_t srcoff, uint64_t len,
	     uint64_t dstoff);
  int truncate(uint64_t offset);

  void encode(bufferlist& bl) const;
  void decode(bufferlist::const_iterator& p);

  seastar::future<OmapIterator> get_iterator() {
    return seastar::make_ready_future<OmapIterator>
      (std::move(OmapIterator(this)));
  }
};
}
