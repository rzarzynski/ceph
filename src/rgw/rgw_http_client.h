// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_HTTP_CLIENT_H
#define CEPH_RGW_HTTP_CLIENT_H

#include "common/RWLock.h"
#include "common/Cond.h"
#include "include/atomic.h"
#include "rgw_common.h"

using param_pair_t = pair<string, string>;
using param_vec_t = vector<param_pair_t>;

struct rgw_http_req_data;

class RGWHTTPClient
{
  friend class RGWHTTPManager;

  bufferlist send_bl;
  bufferlist::iterator send_iter;
  size_t send_len;
  bool has_send_len;
  long http_status;

  rgw_http_req_data *req_data;

  void *user_info;

  string last_method;
  string last_url;
  bool verify_ssl; // Do not validate self signed certificates, default to false

  atomic_t stopped;

protected:
  CephContext *cct;

  param_vec_t headers;
  int init_request(const char *method, const char *url, rgw_http_req_data *req_data);
public:

  static const long HTTP_STATUS_NOSTATUS     = 0;
  static const long HTTP_STATUS_UNAUTHORIZED = 401;

  virtual ~RGWHTTPClient();
  explicit RGWHTTPClient(CephContext *_cct)
    : send_len(0),
      has_send_len(false),
      http_status(HTTP_STATUS_NOSTATUS),
      req_data(nullptr),
      user_info(nullptr),
      verify_ssl(true),
      cct(_cct) {
  }

  void set_user_info(void *info) {
    user_info = info;
  }

  void *get_user_info() {
    return user_info;
  }

  void append_header(const string& name, const string& val) {
    headers.push_back(pair<string, string>(name, val));
  }

  virtual int receive_header(void *ptr, size_t len) {
    return 0;
  }
  virtual int receive_data(void *ptr, size_t len) {
    return 0;
  }
  virtual int send_data(void *ptr, size_t len) {
    return 0;
  }

  void set_send_length(size_t len) {
    send_len = len;
    has_send_len = true;
  }


  long get_http_status() const {
    return http_status;
  }

  void set_verify_ssl(bool flag) {
    verify_ssl = flag;
  }

  int process(const char *method, const char *url);
  int process(const char *url) { return process("GET", url); }

  int wait();
  rgw_http_req_data *get_req_data() { return req_data; }

  string to_str();

  int get_req_retcode();
};


class RGWHTTPHeadersCollector : public RGWHTTPClient {
public:
  /* Case insensitive comparator for containers carrying HTTP headers. */
  struct CILess : public std::binary_function<std::string, std::string, bool> {
    bool operator()(const std::string& lhs,
                    const std::string& rhs) const {
      return ::strcasecmp(lhs.c_str(), rhs.c_str()) < 0 ;
    }
  };

  typedef std::string header_name_t;
  typedef std::string header_value_t;
  typedef std::set<header_name_t, CILess> header_spec_t;

  RGWHTTPHeadersCollector(CephContext * const cct,
                          const header_spec_t relevant_headers)
    : RGWHTTPClient(cct),
      relevant_headers(relevant_headers) {
  }

  int receive_header(void *ptr, size_t len) override;
  int receive_data(void *ptr, size_t len) override {
    return 0;
  }

  int send_data(void *ptr, size_t len) override {
    return 0;
  }

  std::map<header_name_t, header_value_t, CILess> get_headers() const {
    return found_headers;
  }

  /* Throws std::out_of_range */
  const header_value_t& get_header_value(const header_name_t& name) const {
    return found_headers.at(name);
  }


protected:
  const std::set<header_name_t, CILess> relevant_headers;
  std::map<header_name_t, header_value_t, CILess> found_headers;
};


#if 1
// FIXME!
#define dout_subsys ceph_subsys_rgw
class RGWPostHTTPData : public RGWHTTPClient {
  bufferlist *bl;
  std::string post_data;
  size_t post_data_index;
  std::string subject_token;
public:
  RGWPostHTTPData(CephContext *_cct, bufferlist *_bl) : RGWHTTPClient(_cct), bl(_bl), post_data_index(0) {}
  RGWPostHTTPData(CephContext *_cct, bufferlist *_bl, bool verify_ssl) : RGWHTTPClient(_cct), bl(_bl), post_data_index(0){
    set_verify_ssl(verify_ssl);
  }

  void set_post_data(const std::string& _post_data) {
    this->post_data = _post_data;
  }

  int send_data(void* ptr, size_t len) {
    int length_to_copy = 0;
    if (post_data_index < post_data.length()) {
      length_to_copy = min(post_data.length() - post_data_index, len);
      memcpy(ptr, post_data.data() + post_data_index, length_to_copy);
      post_data_index += length_to_copy;
    }
    return length_to_copy;
  }

  int receive_data(void *ptr, size_t len) {
    bl->append((char *)ptr, len);
    return 0;
  }

  int receive_header(void *ptr, size_t len) {
    char line[len + 1];

    char *s = (char *)ptr, *end = (char *)ptr + len;
    char *p = line;
    ldout(cct, 20) << "RGWPostHTTPData::receive_header parsing HTTP headers" << dendl;

    while (s != end) {
      if (*s == '\r') {
        s++;
        continue;
      }
      if (*s == '\n') {
        *p = '\0';
        ldout(cct, 20) << "RGWPostHTTPData::receive_header: line="
                       << line << dendl;
        // TODO: fill whatever data required here
        char *l = line;
        char *tok = strsep(&l, " \t:");
        if (tok) {
          while (l && *l == ' ') {
            l++;
          }

          if (strcasecmp(tok, "X-Subject-Token") == 0) {
            subject_token = l;
          }
        }
      }
      if (s != end) {
        *p++ = *s++;
      }
    }
    return 0;
  }

  std::string get_subject_token() {
    return subject_token;
  }
};
#undef dout_subsys
#endif


class RGWCompletionManager;

class RGWHTTPManager {
  CephContext *cct;
  RGWCompletionManager *completion_mgr;
  void *multi_handle;
  bool is_threaded;
  atomic_t going_down;
  atomic_t is_stopped;

  RWLock reqs_lock;
  map<uint64_t, rgw_http_req_data *> reqs;
  list<rgw_http_req_data *> unregistered_reqs;
  map<uint64_t, rgw_http_req_data *> complete_reqs;
  int64_t num_reqs;
  int64_t max_threaded_req;
  int thread_pipe[2];

  void register_request(rgw_http_req_data *req_data);
  void complete_request(rgw_http_req_data *req_data);
  void _complete_request(rgw_http_req_data *req_data);
  void unregister_request(rgw_http_req_data *req_data);
  void _unlink_request(rgw_http_req_data *req_data);
  void unlink_request(rgw_http_req_data *req_data);
  void finish_request(rgw_http_req_data *req_data, int r);
  void _finish_request(rgw_http_req_data *req_data, int r);
  int link_request(rgw_http_req_data *req_data);

  void manage_pending_requests();

  class ReqsThread : public Thread {
    RGWHTTPManager *manager;

  public:
    ReqsThread(RGWHTTPManager *_m) : manager(_m) {}
    void *entry();
  };

  ReqsThread *reqs_thread;

  void *reqs_thread_entry();

  int signal_thread();

public:
  RGWHTTPManager(CephContext *_cct, RGWCompletionManager *completion_mgr = NULL);
  ~RGWHTTPManager();

  int set_threaded();
  void stop();

  int add_request(RGWHTTPClient *client, const char *method, const char *url);
  int remove_request(RGWHTTPClient *client);

  /* only for non threaded case */
  int process_requests(bool wait_for_data, bool *done);

  int complete_requests();
};

#endif
