// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_CLIENT_IO_H
#define CEPH_RGW_CLIENT_IO_H

#include <stdlib.h>

#include "include/types.h"

#include "rgw_common.h"

class RGWClientIOEngine {
protected:
  RGWEnv env;
  int print(const char *format, ...) { return 0; }

public:
  virtual ~RGWClientIOEngine() {};

  virtual int write_data(const char *buf, int len) = 0;
  virtual int read_data(char *buf, int max) = 0;

  virtual void init_env(CephContext *cct) = 0;
  virtual void flush() = 0;
  virtual int send_status(const char *status, const char *status_name) = 0;
  virtual int send_100_continue() = 0;
  virtual int complete_header() = 0;
  virtual int complete_request() = 0;
  virtual int send_content_length(uint64_t len) = 0;
  virtual RGWEnv& get_env() {
    return env;
  }
};

class RGWClientIO {
  bool account;

  size_t bytes_sent;
  size_t bytes_received;

protected:
  RGWClientIOEngine * const engine;

public:
  virtual ~RGWClientIO() {}
  RGWClientIO(RGWClientIOEngine * const _engine)
    : account(false),
      bytes_sent(0),
      bytes_received(0),
      engine(_engine)
  {}

  void init(CephContext *cct) final;
  int print(const char *format, ...) final;
  int write(const char *buf, int len) final;
  int read(char *buf, int max, int *actual) final;

  virtual void flush() {
    return engine->flush();
  }

  virtual int send_status(const char * const status,
                          const char * const status_name) {
    return engine->send_status(status, status_name);
  }

  virtual int send_100_continue() {
    return engine->send_100_continue();
  }

  virtual int complete_header() {
    return engine->complete_header();
  }

  virtual int complete_request() {
    return engine->complete_request();
  }

  virtual int send_content_length(const uint64_t len) {
    return engine->send_content_length(len);
  }

  RGWEnv& get_env() {
    return engine->get_env();
  }

  void set_account(bool _account) {
    account = _account;
  }

  uint64_t get_bytes_sent() { return bytes_sent; }
  uint64_t get_bytes_received() { return bytes_received; }
};

#if 0
class RGWClientIODecorator : public RGWClientIO {
protected:
  RGWClientIO *IMPL;

public:
  RGWClientIODecorator(RGWClientIO * const impl)
    : IMPL(impl) {}

  /* A lot of wrappers */
  void init(CephContext *cct) {
    return IMPL->init(cct);
  }

  int print(const char *format, ...);

  int write(const char *buf, int len) {
    return IMPL->write(buf, len);
  }

  virtual void flush() {
    return IMPL->flush();
  }

  int read(char *buf, int max, int *actual) {
    return IMPL->read(buf, max, actual);
  }

  virtual int send_status(const char *status, const char *status_name) {
    return IMPL->send_status(status, status_name);
  }

  virtual int send_100_continue() {
    return IMPL->send_100_continue();
  }

  virtual int complete_header() {
    return IMPL->complete_header();
  }

  virtual int complete_request() {
    return IMPL->complete_request();
  }

  virtual int send_content_length(uint64_t len) {
    return IMPL->send_content_length(len);
  }

  RGWEnv& get_env() {
    return IMPL->get_env();
  }

  void set_account(bool _account) {
    return IMPL->set_account(_account);
  }

  uint64_t get_bytes_sent() { return IMPL->get_bytes_sent(); }
  uint64_t get_bytes_received() { return IMPL->get_bytes_received(); }
};

class RGWClientIOBufferAware : public RGWClientIODecorator {
protected:
  bufferlist header_data;
  bufferlist data;

  bool header_done;
  bool sent_header;
  bool has_content_length;

  virtual int write_data(const char *buf, int len) {
    if (!header_done) {
      header_data.append(buf, len);
      return len;
    }
    if (!sent_header) {
      data.append(buf, len);
      return len;
    }

    return IMPL->write_data(buf, len);
  }


public:
  int send_content_length(uint64_t len) {
    has_content_length = true;
    IMPL->send_content_length(len);
  };

  int complete_request();
  int complete_header();
};
#endif
#endif
