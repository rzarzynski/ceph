// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_CLIENT_IO_DECOIMPL_H
#define CEPH_RGW_CLIENT_IO_DECOIMPL_H

#include <type_traits>

#include "rgw_common.h"
#include "rgw_client_io.h"

/* Abstract decorator over any implementation of RGWStreamIOEngine. */
template <typename DecorateeT>
class RGWDecoratedStreamIO : public RGWStreamIOEngine {
  template<typename T> friend class RGWDecoratedStreamIO;

  typedef typename std::remove_pointer<DecorateeT>::type DerefedDecorateeT;

  static_assert(std::is_base_of<RGWStreamIOEngine, DerefedDecorateeT>::value,
                "DecorateeT must be a subclass of RGWStreamIOEngine");

  DecorateeT decoratee;

  /* There is an indirection layer over accessing decoratee to share the same
   * code base between dynamic and static decorators. The difference is about
   * what we store internally: pointer to a decorated object versus the whole
   * object itself. */
  template <typename T = void,
            typename std::enable_if<
    std::is_pointer<DecorateeT>::value, T>::type* = nullptr>
  DerefedDecorateeT& get_decoratee() {
    return *decoratee;
  }

  template <typename T = void,
            typename std::enable_if<
    ! std::is_pointer<DecorateeT>::value, T>::type* = nullptr>
  DerefedDecorateeT& get_decoratee() {
    return decoratee;
  }

protected:
  void init_env(CephContext *cct) override {
    return get_decoratee().init_env(cct);
  }

  int read_data(char* const buf, const int max) override {
    return get_decoratee().read_data(buf, max);
  }

  int write_data(const char* const buf, const int len) override {
    return get_decoratee().write_data(buf, len);
  }

public:
  RGWDecoratedStreamIO(const DecorateeT& decoratee)
    : decoratee(decoratee) {
  }

  int send_status(const int status, const char* const status_name) override {
    return get_decoratee().send_status(status, status_name);
  }

  int send_100_continue() override {
    return get_decoratee().send_100_continue();
  }

  int send_content_length(const uint64_t len) override {
    return get_decoratee().send_content_length(len);
  }

  int complete_header() override {
    return get_decoratee().complete_header();
  }

  void flush() override {
    return get_decoratee().flush();
  }

  RGWEnv& get_env() override {
    return get_decoratee().get_env();
  }

  int complete_request() override {
    return get_decoratee().complete_request();
  }
};


template <typename T>
class RGWStreamIOAccountingEngine : public RGWDecoratedStreamIO<T>,
                                    public RGWClientIOAccounter {
  bool enabled;
  uint64_t total_sent;
  uint64_t total_received;

protected:
  int read_data(char* const buf, const int max) override {
    const auto received = RGWDecoratedStreamIO<T>::read_data(buf, max);
    if (enabled) {
      total_received += received;
    }
    return received;
  }

  int write_data(const char* const buf, const int len) override {
    const auto sent = RGWDecoratedStreamIO<T>::write_data(buf, len);
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

public:
  template <typename U>
  RGWStreamIOAccountingEngine(U&& decoratee)
    : RGWDecoratedStreamIO<T>(std::move(decoratee)),
      enabled(false),
      total_sent(0),
      total_received(0) {
  }

  int send_status(const int status, const char* const status_name) override {
    const auto sent = RGWDecoratedStreamIO<T>::send_status(status, status_name);
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  int send_100_continue() override {
    const auto sent = RGWDecoratedStreamIO<T>::send_100_continue();
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  int send_content_length(const uint64_t len) override {
    const auto sent = RGWDecoratedStreamIO<T>::send_content_length(len);
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  int complete_header() override {
    const auto sent = RGWDecoratedStreamIO<T>::complete_header();
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  uint64_t get_bytes_sent() const override {
    return total_sent;
  }

  uint64_t get_bytes_received() const override {
    return total_received;
  }

  void set_account(bool enabled) override {
    this->emabled = enabled;
  }
};

template <typename T>
RGWStreamIOAccountingEngine<T> add_accounting(T&& t) {
  return RGWStreamIOAccountingEngine<T>(std::move(t));
}


/* Filter for in-memory buffering incoming data and calculating the content
 * length header if it isn't present. */
template <typename T>
class RGWStreamIOBufferingEngine : public RGWDecoratedStreamIO<T> {
  template<typename Td> friend class RGWDecoratedStreamIO;
protected:
  ceph::bufferlist data;

  bool has_content_length;
  bool buffer_data;

  int write_data(const char* buf, const int len) override;

public:
  template <typename U>
  RGWStreamIOBufferingEngine(U&& decoratee)
    : RGWDecoratedStreamIO<T>(std::move(decoratee)),
      has_content_length(false),
      buffer_data(false) {
  }

  int send_content_length(const uint64_t len) override;
  int complete_header() override;
  int complete_request() override;
};

template <typename T>
int RGWStreamIOBufferingEngine<T>::write_data(const char* buf,
                                              const int len)
{
  if (buffer_data) {
    data.append(buf, len);
    return 0;
  }

  return RGWDecoratedStreamIO<T>::write_data(buf, len);
}

template <typename T>
int RGWStreamIOBufferingEngine<T>::send_content_length(const uint64_t len)
{
  has_content_length = true;
  return RGWDecoratedStreamIO<T>::send_content_length(len);
}

template <typename T>
int RGWStreamIOBufferingEngine<T>::complete_header()
{
  if (! has_content_length) {
    /* We will dump everything in complete_request(). */
    buffer_data = true;
    return 0;
  }

  return RGWDecoratedStreamIO<T>::complete_header();
}

template <typename T>
int RGWStreamIOBufferingEngine<T>::complete_request()
{
  size_t sent = 0;

  if (! has_content_length) {
    sent += RGWDecoratedStreamIO<T>::send_content_length(data.length());
    sent += RGWDecoratedStreamIO<T>::complete_header();
  }

  if (buffer_data) {
    if (data.length()) {
      sent += RGWDecoratedStreamIO<T>::write_data(data.c_str(),
                                                  data.length());
      data.clear();
    }

    buffer_data = false;
  }

  return sent + RGWDecoratedStreamIO<T>::complete_request();
}

template <typename T>
RGWStreamIOBufferingEngine<T> add_buffering(T&& t) {
  return RGWStreamIOBufferingEngine<T>(std::move(t));
}


#endif /* CEPH_RGW_CLIENT_IO_DECOIMPL_H */
