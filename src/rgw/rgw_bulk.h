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

class RGWBulkDeleter {
public:
  struct acct_path_t {
    std::string bucket_name;
    rgw_obj_key obj_key;
  };

  struct fail_desc_t {
    int errno;
    rgw_obj_key object;
  };

protected:
  unsigned int num_deleted;
  unsigned int num_unfound;
  std::list<acct_path_t> failures;

  RGWRados * const store;
  RGWObjectCtx * const obj_ctx

public:
  RGWBulkDeleter(RGWRados * const s, RGWObjectCtx * const c)
    : num_deleted(0),
      num_unfound(0),
      store(s),
      obj_ctx(c) {
  }

  unsigned int get_num_deleted() {
    return num_deleted;
  }

  unsigned int get_num_unfound() {
    return num_unfound;
  }

  bool delete_chunk(const std::list<acct_path_t> paths) {
    for (auto path : paths) {
      RGWBucketInfo binfo;
      ret = store->get_bucket_info(obj_ctx, path.bucket_name, binfo, NULL);
      if (ret == -ENOENT) {
        num_unfound++;
        continue;
      } else if (ret < 0) {
        failures.append({ ret, path });
        continue;
      }

      rgw_obj obj(binfo.bucket, path.obj_key);
      obj_ctx->set_atomic(obj);

      RGWRados::Object del_target(store, binfo, obj_ctx, obj);
      RGWRados::Object::Delete del_op(&del_target);

      del_op.params.bucket_owner = binfo.owner;
      del_op.params.versioning_status = binfo.versioning_status();
      del_op.params.obj_owner = ACLOwner();

      ret = del_op.delete_obj();
      if (ret == -ENOENT) {
        num_unfound++;
      } else if (ret < 0) {
        failures.append({ ret, path });
      } else {
        num_deleted++;
      }
    }

    return true;
  }

};

#endif /* RGW_BULK_H_ */
