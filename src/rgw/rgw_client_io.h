// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_CLIENT_IO_H
#define CEPH_RGW_CLIENT_IO_H

#include <stdlib.h>

#include "include/types.h"

#include "rgw_common.h"

class RGWClientIOEngine;
class RGWClientIOEngineDecorator;


class RGWClientIOEngine {
public:
  virtual ~RGWClientIOEngine() {};

  virtual int write_data(const char *buf, int len) = 0;
  virtual int read_data(char *buf, int max) = 0;

  virtual void init_env(CephContext *cct) = 0;
  virtual void flush(RGWClientIO * controller) = 0;
  virtual int send_status(RGWClientIO * const controller,
                          const char * const status,
                          const char *status_name) = 0;
  virtual int send_100_continue(RGWClientIO * const controller) = 0;
  virtual int complete_header(RGWClientIO * const controller) = 0;
  virtual int complete_request(RGWClientIO * const controller) = 0;
  virtual int send_content_length(RGWClientIO * const controller, uint64_t len) = 0;
  virtual RGWEnv& get_env() = 0;

  friend RGWClientIOEngineDecorator;
};

class RGWClientIO {
  bool account;

  size_t bytes_sent;
  size_t bytes_received;

protected:
  RGWClientIOEngine *engine;

public:
  virtual ~RGWClientIO() {}
  RGWClientIO(RGWClientIOEngine * const _engine)
    : account(false),
      bytes_sent(0),
      bytes_received(0),
      engine(_engine)
  {}

  void init(CephContext *cct);
  int print(const char *format, ...);
  int write(const char *buf, int len);
  int read(char *buf, int max, int *actual);

  RGWEnv& get_env() {
    return engine->get_env();
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
  virtual void flush() {
    engine->flush(this);
  }

  virtual int send_status(const char * const status,
                          const char * const status_name) {
    return engine->send_status(this, status, status_name);
  }

  virtual int send_100_continue() {
    return engine->send_100_continue(this);
  }

  virtual int complete_header() {
    return engine->complete_header(this);
  }

  virtual int complete_request() {
    return engine->complete_request(this);
  }

  virtual int send_content_length(const uint64_t len) {
    return engine->send_content_length(this, len);
  }
};


#if 1
class RGWClientIOEngineDecorator : public RGWClientIOEngine {
public:
  RGWClientIOEngine * const IMPL;

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

  RGWClientIOEngineDecorator(RGWClientIOEngine * const impl)
    : IMPL(impl) {}

  /* A lot of wrappers */
  virtual void flush(RGWClientIO * const controller) {
    return IMPL->flush(controller);
  }

  virtual int send_status(RGWClientIO * const controller,
                          const char * const status,
                          const char * const status_name) override {
    return IMPL->send_status(controller, status, status_name);
  }

  virtual int send_100_continue(RGWClientIO * const controller) override {
    return IMPL->send_100_continue(controller);
  }

  virtual int complete_header(RGWClientIO * const controller) override {
    return IMPL->complete_header(controller);
  }

  virtual int complete_request(RGWClientIO * const controller) override {
    return IMPL->complete_request(controller);
  }

  virtual int send_content_length(RGWClientIO * const controller,
                                  const uint64_t len) override {
    return IMPL->send_content_length(controller, len);
  }
};

class RGWClientIOEngineBufferAware : public RGWClientIOEngineDecorator {
protected:
  bufferlist header_data;
  bufferlist data;

  bool header_done;
  bool sent_header;
  bool has_content_length;

  virtual int write_data(const char *buf, const int len) override;

public:
  int send_content_length(RGWClientIO * const controller,
                          const uint64_t len) override;
  int complete_request(RGWClientIO * const controller) override;
  int complete_header(RGWClientIO * const controller) override;
};
#endif
#endif
