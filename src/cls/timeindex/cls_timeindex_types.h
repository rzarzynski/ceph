#ifndef CEPH_CLS_TIMEINDEX_TYPES_H
#define CEPH_CLS_TIMEINDEX_TYPES_H

#include "include/encoding.h"
#include "include/types.h"

#include "include/utime.h"

class JSONObj;


struct cls_timeindex_entry {
  /* Mandatory timestamp. Will be part of the key. */
  utime_t key_ts;
  /* Not mandatory. The name_ext field, if not empty, will form second
   * part of the key. */
  string key_ext;
  /* Become value of OMAP-based mapping. */
  bufferlist value;

  cls_timeindex_entry() {}

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(key_ts, bl);
    ::encode(key_ext, bl);
    ::encode(value, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(1, bl);
    ::decode(key_ts, bl);
    ::decode(key_ext, bl);
    ::decode(value, bl);
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(cls_timeindex_entry)

struct cls_timeindex_header {
  string max_marker;
  utime_t max_time;

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(max_marker, bl);
    ::encode(max_time, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(1, bl);
    ::decode(max_marker, bl);
    ::decode(max_time, bl);
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(cls_timeindex_header)


#endif /* CEPH_CLS_TIMEINDEX_TYPES_H */
