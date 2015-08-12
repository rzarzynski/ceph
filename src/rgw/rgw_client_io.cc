// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "rgw_client_io.h"

#define dout_subsys ceph_subsys_rgw

void RGWClientIO::init(CephContext *cct) {
  engine->init_env(cct);

  if (cct->_conf->subsys.should_gather(ceph_subsys_rgw, 20)) {
    std::map<string, string, ltstr_nocase>& env_map = get_env().get_map();
    std::map<string, string, ltstr_nocase>::iterator iter = env_map.begin();

    for (iter = env_map.begin(); iter != env_map.end(); ++iter) {
      ldout(cct, 20) << iter->first << "=" << iter->second << dendl;
    }
  }
}


int RGWClientIO::print(const char *format, ...)
{
#define LARGE_ENOUGH 128
  int size = LARGE_ENOUGH;

  va_list ap;

  while(1) {
    char buf[size];
    va_start(ap, format);
    int ret = vsnprintf(buf, size, format, ap);
    va_end(ap);

    if (ret >= 0 && ret < size) {
      return write(buf, ret);
    }

    if (ret >= 0) {
      size = ret + 1;
    } else {
      size *= 2;
    }
  }

  /* not reachable */
}

int RGWClientIO::write(const char *buf, int len)
{
  int ret = engine->write_data(buf, len);
  if (ret < 0) {
    return ret;
  }

  if (account) {
    bytes_sent += ret;
  }

  if (ret < len) {
    /* sent less than tried to send, error out */
    return -EIO;
  }

  return 0;
}

int RGWClientIO::read(char *buf, int max, int *actual)
{
  int ret = engine->read_data(buf, max);
  if (ret < 0) {
    return ret;
  }

  *actual = ret;

  bytes_received += *actual;

  return 0;
}


#if 0
int RGWClientIOBufferAware::send_content_length(const uint64_t len) {
  has_content_length = true;
  return IMPL->send_content_length(len);
}

int RGWClientIOBufferAware::complete_header()
{
  header_done = true;

  if (!has_content_length) {
    return 0;
  }

  int rc = write_data(header_data.c_str(), header_data.length());
  sent_header = true;

  return rc;
}

int RGWClientIOBufferAware::complete_request()
{
  if (!sent_header) {
    if (!has_content_length) {
      header_done = false; /* let's go back to writing the header */

      if (0 && data.length() == 0) {
        has_content_length = true;
        print("Transfer-Enconding: %s\r\n", "chunked");
        data.append("0\r\n\r\n", sizeof("0\r\n\r\n")-1);
      } else {
        int r = send_content_length(data.length());
        if (r < 0) {
	        return r;
        }
      }
    }
    complete_header();
  }

  if (data.length()) {
    int r = write_data(data.c_str(), data.length());
    if (r < 0) {
      return r;
    }
    data.clear();
  }

  return 0;
}
#endif
