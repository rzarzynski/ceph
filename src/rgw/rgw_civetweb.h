// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_MONGOOSE_H
#define CEPH_RGW_MONGOOSE_H
#define TIME_BUF_SIZE 128

#include "rgw_client_io.h"


struct mg_connection;


class RGWMongoose : public RGWClientIO
{
  mg_connection *conn;

  int port;

  bool explicit_keepalive;
  bool explicit_conn_close;

public:
  RGWMongoose(mg_connection *_conn, int _port);

  void init_env(CephContext *cct) override;

  int write_data(const char *buf, int len) override;
  int read_data(char *buf, int len) override;

  int send_status(const char *status, const char *status_name) override;
  int send_100_continue() override;
  int complete_header() override;
  int complete_request() override { return 0; }
  int send_content_length(uint64_t len) override;

  void flush() override;
};


#endif
