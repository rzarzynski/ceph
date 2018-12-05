#ifndef CEPH_PAGE_H
#define CEPH_PAGE_H

#include "include/intarith.h"

namespace ceph {
  // these are in common/page.cc
  extern ceph::math::p2_uint32_t _page_size;
  extern unsigned _page_mask;
}

#endif


#define CEPH_PAGE_SIZE (ceph::_page_size.get_value())
#define CEPH_PAGE_MASK (ceph::_page_mask)
#define CEPH_PAGE_SHIFT (ceph::_page_size.get_exponent())


