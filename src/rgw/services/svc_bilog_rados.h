
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

#include "cls_fifo_legacy.h"
#include "rgw/rgw_service.h"

#include "svc_rados.h"




class RGWSI_BILog_RADOS : public RGWServiceInstance
{
public:
  virtual ~RGWSI_BILog_RADOS() {}

  RGWSI_BILog_RADOS(CephContext *cct);

  virtual void init(RGWSI_BucketIndex_RADOS *bi_rados_svc) = 0;

  virtual int log_start(const RGWBucketInfo& bucket_info, int shard_id) = 0;
  virtual int log_stop(const RGWBucketInfo& bucket_info, int shard_id) = 0;

  virtual int log_trim(const RGWBucketInfo& bucket_info,
                       int shard_id,
                       std::string& marker) = 0;
  virtual int log_list(const RGWBucketInfo& bucket_info,
                       int shard_id,
                       std::string& marker,
                       uint32_t max,
                       std::list<rgw_bi_log_entry>& result,
                       bool *truncated) = 0;

  virtual int log_get_max_marker(const RGWBucketInfo& bucket_info,
                                 const std::map<int, rgw_bucket_dir_header>& headers,
                                 int shard_id,
                                 std::map<int, std::string> *max_markers) = 0;
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
               std::string& marker) override;
  int log_list(const RGWBucketInfo& bucket_info,
               int shard_id,
               std::string& marker,
               uint32_t max,
               std::list<rgw_bi_log_entry>& result,
               bool *truncated) override;

  int log_get_max_marker(const RGWBucketInfo& bucket_info,
                         const std::map<int, rgw_bucket_dir_header>& headers,
                         int shard_id,
                         std::map<int, std::string> *max_markers) override;
};

class RGWSI_BILog_RADOS_CLSFIFO : public RGWSI_BILog_RADOS
{
  static constexpr char BILOG_FIFO_SUFFIX[] = ".bilog_fifo";

  std::unique_ptr<rgw::cls::fifo::FIFO> _open_fifo(
    const RGWBucketInfo& bucket_info);

  friend struct BILogUpdateBatchFIFO;

public:
  using RGWSI_BILog_RADOS::RGWSI_BILog_RADOS;

  void init(RGWSI_BucketIndex_RADOS *bi_rados_svc) override;

  int log_start(const RGWBucketInfo& bucket_info, int shard_id) override;
  int log_stop(const RGWBucketInfo& bucket_info, int shard_id) override;

  int log_trim(const RGWBucketInfo& bucket_info,
               int shard_id,
               std::string& marker) override;
  int log_list(const RGWBucketInfo& bucket_info,
               int shard_id,
               std::string& marker,
               uint32_t max,
               std::list<rgw_bi_log_entry>& result,
               bool *truncated) override;

  int log_get_max_marker(const RGWBucketInfo& bucket_info,
                         const std::map<int, rgw_bucket_dir_header>& headers,
                         int shard_id,
                         std::map<int, string> *max_markers) override;
};
