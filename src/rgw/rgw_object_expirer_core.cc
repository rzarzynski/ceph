// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <errno.h>
#include <iostream>
#include <sstream>
#include <string>

using namespace std;

#include "auth/Crypto.h"

#include "common/armor.h"
#include "common/ceph_json.h"
#include "common/config.h"
#include "common/ceph_argparse.h"
#include "common/Formatter.h"
#include "common/errno.h"

#include "global/global_init.h"

#include "include/utime.h"
#include "include/str_list.h"

#include "rgw_user.h"
#include "rgw_bucket.h"
#include "rgw_rados.h"
#include "rgw_acl.h"
#include "rgw_acl_s3.h"
#include "rgw_log.h"
#include "rgw_formats.h"
#include "rgw_usage.h"
#include "rgw_replica_log.h"
#include "rgw_object_expirer_core.h"

#define dout_subsys ceph_subsys_rgw

int ObjectExpirer::init_bucket_info(const string& bucket_name,
                                    const string& bucket_id,
                                    RGWBucketInfo& bucket_info)
{
  RGWObjectCtx obj_ctx(store);
  const string bucket_instance_id = bucket_name + ":" + bucket_id;

  int ret = store->get_bucket_instance_info(obj_ctx, bucket_instance_id,
          bucket_info, NULL, NULL);

  return ret;
}

int ObjectExpirer::garbage_single_object(objexp_hint_entry& hint)
{
  RGWBucketInfo bucket_info;

  int ret = init_bucket_info(hint.bucket_name, hint.bucket_id, bucket_info);
  if (ret < 0) {
    dout(1) << "ERROR: could not init bucket: " << cpp_strerror(-ret) << dendl;
    return ret;
  }

  RGWObjectCtx rctx(store);

  rgw_obj_key key = hint.obj_key;
  if (key.instance.empty()) {
    key.instance = "null";
  }

  rgw_obj obj(bucket_info.bucket, key);
  store->set_atomic(&rctx, obj);
  ret = store->delete_obj(rctx, bucket_info, obj,
          bucket_info.versioning_status(), 0, hint.exp_time);

  return ret;
}

void ObjectExpirer::garbage_chunk(list<cls_timeindex_entry>& entries,      /* in  */
                                  bool& need_trim)                         /* out */
{
  need_trim = false;

  for (list<cls_timeindex_entry>::iterator iter = entries.begin();
       iter != entries.end();
       ++iter)
  {
    objexp_hint_entry hint;
    dout(15) << "got removal hint for: " << iter->key_ts.sec() \
        << " - " << iter->key_ext << dendl;

    int ret = store->objexp_hint_parse(*iter, hint);
    if (ret < 0) {
      dout(1) << "cannot parse removal hint for " << hint.obj_key << dendl;
      continue;
    }

    /* PRECOND_FAILED simply means that our hint is not valid.
     * We can silently ignore that and move forward. */
    ret = garbage_single_object(hint);
    if (ret == -ERR_PRECONDITION_FAILED) {
      dout(15) << "not actual hint for object: " << hint.obj_key << dendl;
    } else if (ret < 0) {
      dout(1) << "cannot remove expired object: " << hint.obj_key << dendl;
    }

    need_trim = true;
  }

  return;
}

void ObjectExpirer::trim_chunk(const string& shard,
                               const utime_t& from,
                               const utime_t& to)
{
  dout(20) << "trying to trim removal hints to  " << to << dendl;

  int ret = store->objexp_hint_trim(shard, from, to);
  if (ret < 0) {
    dout(0) << "ERROR during trim: " << ret << dendl;
  }

  return;
}

void ObjectExpirer::proceed_single_shard(const string& shard,
                                         const utime_t& last_run,
                                         const utime_t& round_start)
{
  string marker;
  string out_marker;
  bool truncated = false;

  do {
    list<cls_timeindex_entry> entries;
    int ret = store->objexp_hint_list(shard, last_run, round_start,
                                      1000, marker, entries,
                                      &out_marker, &truncated);
    if (ret < 0) {
      dout(10) << "cannot get removal hints from shard: " << shard << dendl;
      continue;
    }

    bool need_trim;
    garbage_chunk(entries, need_trim);

    if (need_trim) {
      trim_chunk(shard, last_run, round_start);
    }

    marker = out_marker;
  } while (truncated);

  return;
}

void ObjectExpirer::inspect_all_shards(const utime_t& last_run,
                                       const utime_t& round_start)
{
  bool is_next_available;
  utime_t shard_marker;

  do {
    string shard;
    store->objexp_get_shard(last_run, round_start, shard_marker, shard,
            is_next_available);

    dout(20) << "proceeding shard = " << shard << dendl;

    proceed_single_shard(shard, last_run, round_start);
  } while (is_next_available);

  return;
}
