// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * This file contains declaration of backend classes targeting bulk
 * operations of Swift API. We need to keep them separated from users
 * in order to facilitate reusage-through-composition across multiple 
 * RWGOps which need such feature.
 * It's also worth to note that radosgw already has similar entities
 * for S3 API (see RGWMultiDelete class). Unfortunately, Swift is so
 * different here (eg. ability to delete objects from multiple buckets)
 * that deeply reworking them doesn't make any sense. 
 */

#ifndef RGW_BULK_H_
#define RGW_BULK_H_

#include <vector>
#include "rgw_xml.h"
#include "rgw_common.h"

#define dout_subsys ceph_subsys_rgw

class RGWBulkDeleter {
public:
  struct acct_path_t {
    std::string bucket_name;
    rgw_obj_key obj_key;
  };

  struct fail_desc_t {
    int err;
    acct_path_t path;
  };

protected:
  unsigned int num_deleted;
  unsigned int num_unfound;
  std::list<fail_desc_t> failures;

  RGWRados * const store;
  req_state * const s;

public:
  RGWBulkDeleter(RGWRados * const str, req_state * const s)
    : num_deleted(0),
      num_unfound(0),
      store(str),
      s(s) {
  }

  unsigned int get_num_deleted() {
    return num_deleted;
  }

  unsigned int get_num_unfound() {
    return num_unfound;
  }

  bool verify_permission(RGWBucketInfo& binfo,
                         map<string, bufferlist>& battrs,
                         rgw_obj& obj,
                         ACLOwner& bucket_owner /* out */) {
    int ret = 0;

    RGWAccessControlPolicy bacl(store->ctx());
    rgw_obj_key no_obj;
    //ret = read_policy(store, s, binfo, battrs, &bacl, binfo.bucket, no_obj);
    if (ret < 0) {
      return false;
    }

    bucket_owner = bacl.get_owner();

    RGWAccessControlPolicy oacl(s->cct);
    //ret = read_policy(store, s, binfo, battrs, &oacl, binfo.bucket, s->object);
    if (ret < 0) {
      return false;
    }

    return verify_object_permission(s, &bacl, &oacl, RGW_PERM_WRITE);
  }

  bool delete_single(const acct_path_t path) {
    int ret = 0;

    auto& obj_ctx = *static_cast<RGWObjectCtx *>(s->obj_ctx);
    RGWBucketInfo binfo;
    map<string, bufferlist> battrs;
    ret = store->get_bucket_info(obj_ctx, path.bucket_name, binfo, NULL, &battrs);
    if (ret < 0) {
      goto binfo_fail;
    }

    /* We do need a new scope due to goto. */
    {
      rgw_obj obj(binfo.bucket, path.obj_key);
      obj_ctx.set_atomic(obj);

      RGWRados::Object del_target(store, binfo, obj_ctx, obj);
      RGWRados::Object::Delete del_op(&del_target);

      del_op.params.bucket_owner = binfo.owner;
      del_op.params.versioning_status = binfo.versioning_status();
      del_op.params.obj_owner = ACLOwner();


      ret = del_op.delete_obj();

      if (ret < 0) {
        goto delop_fail;
      }
    }

    num_deleted++;
    return true;

binfo_fail:
      if (-ENOENT == ret) {
        ldout(store->ctx(), 20) << "cannot find bucket = " << path.bucket_name << dendl;
        num_unfound++;
      } else {
        ldout(store->ctx(), 20) << "cannot get bucket info, ret = " << ret << dendl;

        fail_desc_t failed_item = {
          .err  = ret,
          .path = path
        };
        failures.push_back(failed_item);
      }
      return false;

delop_fail:
      if (-ENOENT == ret) {
        ldout(store->ctx(), 20) << "cannot find the object" << dendl;
        num_unfound++;
      } else {
        fail_desc_t failed_item = {
          .err  = ret,
          .path = path
        };
        failures.push_back(failed_item);
      }
      return false;
  }

  bool delete_chunk(const std::list<acct_path_t> paths) {
    ldout(store->ctx(), 20) << "in delete_chunk" << dendl;
    for (auto path : paths) {
      ldout(store->ctx(), 20) << "bulk deleting path: " << path.bucket_name << dendl;
      delete_single(path);
    }

    return true;
  }

};

#endif /* RGW_BULK_H_ */
