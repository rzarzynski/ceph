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
    std::map<string, string, ltstr_nocase>& env_map = \
        engine->get_env().get_map();
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


int RGWClientIOEngineBufferAware::write_data(const char *buf, int len) {
  if (buffer_data) {
    data.append(buf, len);
    return len;
  }

  return RGWClientIOEngineDecorator::write_data(buf, len);
}

int RGWClientIOEngineBufferAware::send_content_length(RGWClientIO * const controller,
                                                      const uint64_t len) {
  has_content_length = true;
  return RGWClientIOEngineDecorator::send_content_length(controller, len);
}

int RGWClientIOEngineBufferAware::complete_header(RGWClientIO * const controller)
{
  if (!has_content_length) {
    /* We will dump everything in complete_request(). */
    buffer_data = true;
    return 0;
  }

  return RGWClientIOEngineDecorator::complete_header(controller);
}

int RGWClientIOEngineBufferAware::complete_request(RGWClientIO * const controller)
{
  if (buffer_data) {
    buffer_data = false;

    send_content_length(controller, data.length());
    RGWClientIOEngineDecorator::complete_header(controller);

    if (data.length()) {
      int ret = RGWClientIOEngineDecorator::write_data(data.c_str(), data.length());
      if (ret < 0) {
        return r;
      }
      data.clear();
    }
  }

  return RGWClientIOEngineDecorator::complete_request(controller);
}
