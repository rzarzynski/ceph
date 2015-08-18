// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_MONGOOSE_H
#define CEPH_RGW_MONGOOSE_H
#define TIME_BUF_SIZE 128

#include "rgw_client_io.h"


struct mg_connection;


class RGWMongoose : public RGWClientIOEngine
{
  mg_connection * const conn;
  RGWEnv env;

  bufferlist early_header_data;
  bufferlist header_data;

  int port;

  enum {
    RGW_CIVETWEB_EARLY_HEADERS, /* Headers sent before calling send_status. */
    RGW_CIVETWEB_STATUS_SEEN,   /* Status has been seen. */
    RGW_CIVETWEB_DATA           /* Header has been completed. */
  } phase;
  bool has_content_length;
  bool explicit_keepalive;
  bool explicit_conn_close;

public:
  RGWMongoose(mg_connection *_conn, int _port);

  void init_env(CephContext *cct) override;
  int write_data(const char *buf, int len) override;
  int read_data(char *buf, int len) override;

  void flush(RGWClientIO * controller) override;
  int send_status(RGWClientIO * const controller,
                  const char *status,
                  const char *status_name) override;
  int send_100_continue(RGWClientIO * const controller) override;
  int complete_header(RGWClientIO * const controller) override;
  int send_content_length(RGWClientIO * const controller,
                          uint64_t len) override;

  int complete_request(RGWClientIO * const controller) override {
    return 0;
  }

  RGWEnv& get_env() override {
    return env;
  }
};


#endif
