// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_COMMON_BATCH_H
#define CEPH_COMMON_BATCH_H

namespace ceph {

template <class T>
class continous_batch {
  T* const data;
  size_t n;

public:
  continous_batch(T& single)
    : data(&single),
      n(1) {
  }

  continous_batch(T* const data, const size_t n)
    : data(data),
      n(n) {
  }

  T* begin() {
    return &data[0];
  }
  T* end() {
    return &data[n];
  }

  const T* begin() const {
    return &data[0];
  }
  const T* end() const {
    return &data[n];
  }

  const T* cbegin() const {
    return &data[0];
  }
  const T* cend() const {
    return &data[n];
  }
};

} // namespace ceph

#endif // CEPH_COMMON_BATCH_H
