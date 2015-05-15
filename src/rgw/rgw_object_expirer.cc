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

#define dout_subsys ceph_subsys_rgw

static RGWRados *store = NULL;

class StoreDestructor {
  RGWRados *store;

public:
  StoreDestructor(RGWRados *_s) : store(_s) {}
  ~StoreDestructor() {
    if (store) {
      RGWStoreManager::close_storage(store);
    }
  }
};

static void usage()
{
  generic_client_usage();
}

static inline utime_t get_time_step (void)
{
  //return utime_t(2 << g_ceph_context->_conf->rgw_objexp_exp, 0);
  return utime_t(2 << 12, 0);
}

static inline utime_t get_last_run_time (void)
{
  return utime_t();
}

static void garbage_single_object ()
{
  return;
}

static void garbage_chunk (list<cls_timeindex_entry>& entries)
{
  for (list<cls_timeindex_entry>::iterator iter = entries.begin();
       iter != entries.end();
       ++iter)
  {
    std::cout << "round - key_ext: " << iter->key_ext << std::endl;
  }
  return;
}

static void trim_chunk ()
{
}

int main (int argc, char **argv)
{
  vector<const char *> args;
  argv_to_vec(argc, (const char **)argv, args);
  env_to_vec(args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_DAEMON, 0);
  common_init_finish(g_ceph_context);

  store = RGWStoreManager::get_storage(g_ceph_context, false, false);
  if (!store) {
    std::cerr << "couldn't init storage provider" << std::endl;
    return EIO;
  }

  rgw_user_init(store);
  rgw_bucket_init(store->meta_mgr);

  /* Guard to not forget about closing the rados store. */
  StoreDestructor store_dtor(store);

  utime_t last_run = get_last_run_time();
  while (true) {
    const utime_t round_start = ceph_clock_now(g_ceph_context);
    const utime_t time_step = get_time_step();
    const size_t num_shards = g_ceph_context->_conf->rgw_objexp_hints_num_shards;

    for (struct { utime_t t; size_t i; } v = { last_run, 0 };
         v.t < round_start && v.i < num_shards;
         v.t += time_step, v.i++)
    {
      const string shard = store->objexp_hint_get_shardname(v.t);

      string marker;
      string out_marker;
      bool truncated = false;

      do {
        list<cls_timeindex_entry> entries;

        int ret = store->objexp_hint_list(shard, last_run, round_start,
                                          1000, marker, entries,
                                          &out_marker, &truncated);
        if (ret < 0) {
          std::cout << "round " << v.i << " - " << shard << " ======== ERROR: " << ret << std::endl;
          break;
        }
        garbage_chunk(entries);
        marker = out_marker;
      } while (truncated);
    } /* end for */

    /* End of the real work for now. Prepare for sleep. */
    const utime_t round_time = ceph_clock_now(g_ceph_context) - round_start;
    const utime_t interval(g_ceph_context->_conf->rgw_objexp_gc_interval, 0);

    if (round_time < interval) {
      /* This should be the main path of execution. All currently expired
       * objects have been removed and we need go sleep waiting for the next
       * turn. If the check isn't true, it means we have to much hints
       * in relation to interval time. */
      const utime_t sleep_period = interval - round_time;
      std::cout << "sleeping - " << sleep_period << std::endl;
      sleep_period.sleep();
    }
  }

  return 0;
}
