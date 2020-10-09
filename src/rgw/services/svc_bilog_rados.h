
// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */


#pragma once

#include "rgw/rgw_service.h"

#include "svc_rados.h"




class RGWSI_BILog_RADOS : public RGWServiceInstance
{
public:
  RGWSI_BILog_RADOS(CephContext *cct);

  virtual void init(RGWSI_BucketIndex_RADOS *bi_rados_svc);

  virtual int log_start(const RGWBucketInfo& bucket_info, int shard_id);
  virtual int log_stop(const RGWBucketInfo& bucket_info, int shard_id);

  virtual int log_trim(const RGWBucketInfo& bucket_info,
                       int shard_id,
                       std::string& start_marker,
                       std::string& end_marker);
  virtual int log_list(const RGWBucketInfo& bucket_info,
                       int shard_id,
                       std::string& marker,
                       uint32_t max,
                       std::list<rgw_bi_log_entry>& result,
                       bool *truncated);

  virtual int get_log_status(const RGWBucketInfo& bucket_info,
                             int shard_id,
                             map<int, string> *markers);
};

class RGWSI_BILog_RADOS_InIndex : public RGWSI_BILog_RADOS
{
public:
  struct Svc {
    RGWSI_BucketIndex_RADOS *bi{nullptr};
  } svc;

  using RGWSI_BILog_RADOS::RGWSI_BILog_RADOS;

  void init(RGWSI_BucketIndex_RADOS *bi_rados_svc) override;

  int log_start(const RGWBucketInfo& bucket_info, int shard_id) override;
  int log_stop(const RGWBucketInfo& bucket_info, int shard_id) override;

  int log_trim(const RGWBucketInfo& bucket_info,
               int shard_id,
               std::string& start_marker,
               std::string& end_marker) override;
  int log_list(const RGWBucketInfo& bucket_info,
               int shard_id,
               std::string& marker,
               uint32_t max,
               std::list<rgw_bi_log_entry>& result,
               bool *truncated) override;

  int get_log_status(const RGWBucketInfo& bucket_info,
                     int shard_id,
                     map<int, string> *markers) override;
};
