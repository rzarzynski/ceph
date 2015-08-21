// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_CLIENT_IO_H
#define CEPH_RGW_CLIENT_IO_H

#include <memory>
#include <stdlib.h>

#include "include/types.h"

#include "rgw_common.h"

class RGWClientIOEngine {
public:
  virtual ~RGWClientIOEngine() {};

  virtual int write_data(const char *buf, int len) = 0;
  virtual int read_data(char *buf, int max) = 0;

  virtual void init_env(CephContext *cct) = 0;
  virtual void flush(RGWClientIO * controller) = 0;
  virtual int send_status(RGWClientIO * const controller,
                          const char * const status,
                          const char * const status_name) = 0;
  virtual int send_100_continue(RGWClientIO * const controller) = 0;
  virtual int complete_header(RGWClientIO * const controller) = 0;
  virtual int complete_request(RGWClientIO * const controller) = 0;
  virtual int send_content_length(RGWClientIO * const controller, uint64_t len) = 0;
  virtual RGWEnv& get_env() = 0;
};


class RGWClientIOEngineDecorator : public RGWClientIOEngine {
  std::shared_ptr<RGWClientIOEngine> decorated;

public:
  RGWClientIOEngineDecorator(std::shared_ptr<RGWClientIOEngine> impl)
    : decorated(impl) {
  }

  /* A lot of wrappers */
  virtual void init_env(CephContext *cct) override {
    return decorated->init_env(cct);
  }

  virtual int write_data(const char * const buf,
                         const int len) override {
    return decorated->write_data(buf, len);
  }

  virtual int read_data(char * const buf,
                        const int max) override {
    return decorated->read_data(buf, max);
  }

  virtual void flush(RGWClientIO * const controller) {
    return decorated->flush(controller);
  }

  virtual int send_status(RGWClientIO * const controller,
                          const char * const status,
                          const char * const status_name) override {
    return decorated->send_status(controller, status, status_name);
  }

  virtual int send_100_continue(RGWClientIO * const controller) override {
    return decorated->send_100_continue(controller);
  }

  virtual int complete_header(RGWClientIO * const controller) override {
    return decorated->complete_header(controller);
  }

  virtual int complete_request(RGWClientIO * const controller) override {
    return decorated->complete_request(controller);
  }

  virtual int send_content_length(RGWClientIO * const controller,
                                  const uint64_t len) override {
    return decorated->send_content_length(controller, len);
  }

  virtual RGWEnv& get_env() override {
    return decorated->get_env();
  }
};


class RGWClientIOEngineBufferAware : public RGWClientIOEngineDecorator {
protected:
  bufferlist data;

  bool has_content_length;
  bool buffer_data;

  virtual int write_data(const char *buf, const int len) override;

public:
  RGWClientIOEngineBufferAware(std::shared_ptr<RGWClientIOEngine> engine)
    : RGWClientIOEngineDecorator(engine),
      has_content_length(false),
      buffer_data(false) {
  }

  int send_content_length(RGWClientIO * const controller,
                          const uint64_t len) override;
  int complete_request(RGWClientIO * const controller) override;
  int complete_header(RGWClientIO * const controller) override;
};


class RGWClientIO {
  bool account;

  size_t bytes_sent;
  size_t bytes_received;

protected:
  const std::shared_ptr<RGWClientIOEngine> engine;

  RGWClientIO(const std::shared_ptr<RGWClientIOEngine> engine)
    : account(false),
      bytes_sent(0),
      bytes_received(0),
      engine(engine) {
  }

public:
  class Builder;

  enum class BufferingMode  {
    CONLEN_BUFFER_DATA,
    CONLEN_CHUNK_DATA,
    CONLEN_PASS_DATA
  };

  virtual ~RGWClientIO() {
  }

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


class RGWClientIO::Builder {
protected:
  /* Whether engine is resistant to sending some headers first and then
   * setting HTTP status or not and we need to reorder operations. */
  bool needs_reordering;

  /* Last stage in pipeline. */
  std::shared_ptr<RGWClientIOEngine> final_engine;

public:
  Builder(std::shared_ptr<RGWClientIOEngine> engine)
    : final_engine(engine) {
  }

  RGWClientIO getResult() {
    std::shared_ptr<RGWClientIOEngine> stage = final_engine;

#if 0
    if (needs_reordering) {
      stage = std::make_shared<RGWClientIOEngineReorderer>(stage);
    }
#endif

    stage = std::make_shared<RGWClientIOEngineBufferAware>(stage);

    return RGWClientIO(stage);
  }
};
#endif
