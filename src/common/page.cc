#include <unistd.h>
#include "include/intarith.h"

namespace ceph {
  ceph::math::p2_uint32_t _page_size = \
    ceph::math::p2_uint32_t(sysconf(_SC_PAGESIZE));
  unsigned long _page_mask = ~(unsigned long)(_page_size - 1);
}
