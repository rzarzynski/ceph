// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_CLIENT_IO_H
#define CEPH_RGW_CLIENT_IO_H

#include <stdlib.h>

#include "include/types.h"

#include "rgw_common.h"

class RGWClientIODecorator;


class RGWClientIO {
  bool account;

  size_t bytes_sent;
  size_t bytes_received;

protected:
  RGWEnv env;

  virtual void init_env(CephContext *cct) = 0;
  virtual int write_data(const char *buf, int len) = 0;
  virtual int read_data(char *buf, int max) = 0;

public:
  virtual ~RGWClientIO() {}
  RGWClientIO()
    : account(false),
      bytes_sent(0),
      bytes_received(0)
  {}

  void init(CephContext *cct);
  int print(const char *format, ...);
  int write(const char *buf, int len);
  int read(char *buf, int max, int *actual);

  RGWEnv& get_env() {
    return env;
  }

  void set_account(bool _account) {
    account = _account;
  }

  uint64_t get_bytes_sent() {
    return bytes_sent;
  }

  uint64_t get_bytes_received() {
    return bytes_received;
  }

  /* Public interface parts which must be implemented for concrete
   * frontend provider. */
  virtual void flush() = 0;
  virtual int send_status(const char * const status,
                          const char * const status_name) = 0;
  virtual int send_100_continue() = 0;
  virtual int complete_header() = 0;
  virtual int complete_request() = 0;
  virtual int send_content_length(uint64_t len) = 0;

  friend RGWClientIODecorator;
};


class RGWClientIODecorator : public RGWClientIO {
protected:
  RGWClientIO * const IMPL;

  virtual void init_env(CephContext *cct) override {
    return IMPL->init_env(cct);
  }

  virtual int write_data(const char * const buf,
                         const int len) override {
    return IMPL->write_data(buf, len);
  }

  virtual int read_data(char * const buf,
                        const int max) override {
    return IMPL->read_data(buf, max);
  }

public:
  RGWClientIODecorator(RGWClientIO * const impl)
    : IMPL(impl) {}

  /* A lot of wrappers */
  virtual void flush() {
    return IMPL->flush();
  }

  virtual int send_status(const char * const status,
                          const char * const status_name) override {
    return IMPL->send_status(status, status_name);
  }

  virtual int send_100_continue() override {
    return IMPL->send_100_continue();
  }

  virtual int complete_header() override {
    return IMPL->complete_header();
  }

  virtual int complete_request() override {
    return IMPL->complete_request();
  }

  virtual int send_content_length(const uint64_t len) override {
    return IMPL->send_content_length(len);
  }
};

class RGWClientIOBufferAware : public RGWClientIODecorator {
protected:
  bufferlist header_data;
  bufferlist data;

  bool header_done;
  bool sent_header;
  bool has_content_length;

  virtual int write_data(const char *buf, const int len) override;

public:
  int send_content_length(const uint64_t len) override;
  int complete_request() override;
  int complete_header() override;
};
#endif
