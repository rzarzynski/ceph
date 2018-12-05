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

#ifndef CEPH_INTARITH_H
#define CEPH_INTARITH_H

#include <limits>
#include <type_traits>
#include <cstdint>

#ifdef __CEPH__
# include "include/ceph_assert.h"
#else
# include <assert.h>
#endif

template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> div_round_up(T n, U d) {
  return (n + d - 1) / d;
}


template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> round_up_to(T n, U d) {
  return (n % d ? (n + d - n % d) : n);
}

template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> shift_round_up(T x, U y) {
  return (x + (1 << y) - 1) >> y;
}

/*
 * Wrapper to determine if value is a power of 2
 */
template<typename T>
constexpr inline bool isp2(T x) {
  return (x & (x - 1)) == 0;
}

/*
 * Wrappers for various sorts of alignment and rounding.  The "align" must
 * be a power of 2.  Often times it is a block, sector, or page.
 */

/*
 * return x rounded down to an align boundary
 * eg, p2align(1200, 1024) == 1024 (1*align)
 * eg, p2align(1024, 1024) == 1024 (1*align)
 * eg, p2align(0x1234, 0x100) == 0x1200 (0x12*align)
 * eg, p2align(0x5600, 0x100) == 0x5600 (0x56*align)
 */
template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> p2align(T x, U align) {
  return x & -align;
}

/*
 * return x % (mod) align
 * eg, p2phase(0x1234, 0x100) == 0x34 (x-0x12*align)
 * eg, p2phase(0x5600, 0x100) == 0x00 (x-0x56*align)
 */
template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> p2phase(T x, U align) {
  return x & (align - 1);
}

/*
 * return how much space is left in this block (but if it's perfectly
 * aligned, return 0).
 * eg, p2nphase(0x1234, 0x100) == 0xcc (0x13*align-x)
 * eg, p2nphase(0x5600, 0x100) == 0x00 (0x56*align-x)
 */
template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> p2nphase(T x, U align) {
  return -x & (align - 1);
}

/*
 * return x rounded up to an align boundary
 * eg, p2roundup(0x1234, 0x100) == 0x1300 (0x13*align)
 * eg, p2roundup(0x5600, 0x100) == 0x5600 (0x56*align)
 */
template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> p2roundup(T x, U align) {
  return (-(-(x) & -(align)));
}

// count trailing zeros.
// NOTE: the builtin is nondeterministic on 0 input
template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) <= sizeof(unsigned)),
  unsigned>::type ctz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_ctz(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned int) &&
   sizeof(T) <= sizeof(unsigned long)),
  unsigned>::type ctz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_ctzl(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned long) &&
   sizeof(T) <= sizeof(unsigned long long)),
  unsigned>::type ctz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_ctzll(v);
}

// count leading zeros
// NOTE: the builtin is nondeterministic on 0 input
template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) <= sizeof(unsigned)),
  unsigned>::type clz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_clz(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned int) &&
   sizeof(T) <= sizeof(unsigned long)),
  unsigned>::type clz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_clzl(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned long) &&
   sizeof(T) <= sizeof(unsigned long long)),
  unsigned>::type clz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_clzll(v);
}

// count bits (set + any 0's that follow)
template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) <= sizeof(unsigned)),
  unsigned>::type cbits(T v) {
  if (v == 0)
    return 0;
  return (sizeof(v) * 8) - __builtin_clz(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned int) &&
   sizeof(T) <= sizeof(unsigned long)),
  unsigned>::type cbits(T v) {
  if (v == 0)
    return 0;
  return (sizeof(v) * 8) - __builtin_clzl(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned long) &&
   sizeof(T) <= sizeof(unsigned long long)),
  unsigned>::type cbits(T v) {
  if (v == 0)
    return 0;
  return (sizeof(v) * 8) - __builtin_clzll(v);
}

namespace ceph::math {

template<class UnsignedValueT>
class p2_t {
  typedef std::uint8_t exponent_type;
  typedef UnsignedValueT value_type;

  exponent_type exponent;

  struct _validation_skipper_t {};
  explicit p2_t(const value_type value, _validation_skipper_t)
    : exponent(ctz(value)) {
  }

public:
  explicit p2_t(const value_type value)
    : exponent(ctz(value)) {
    static_assert(std::is_unsigned_v<value_type>);
    static_assert(std::numeric_limits<exponent_type>::max() >
                  std::numeric_limits<value_type>::digits);

    // 0 isn't a power of two. Additional validation is necessary as
    // the isp2 routine doesn't sanitize that case.
    ceph_assert(value != 0);
    ceph_assert(isp2(value));
  }

  template <class T,
            typename = std::enable_if_t<std::is_convertible_v<T, value_type>>>
  explicit p2_t(const p2_t<T>& r)
    : exponent(r.get_exponent()) {
  }

  static p2_t<value_type> from_p2(const value_type p2) {
    return p2_t(p2, _validation_skipper_t());
  }

  static p2_t<value_type> from_exponent(const exponent_type exponent) {
    return p2_t(1 << exponent, _validation_skipper_t());
  }

  exponent_type get_exponent() const {
    return exponent;
  }

  value_type get_value() const {
    return 1 << exponent;
  }

  operator value_type() const {
    return get_value();
  }

  template <typename RightT>
  p2_t<value_type>& operator=(const p2_t<RightT>& r) {
    static_assert(std::is_convertible_v<RightT, value_type>);
    exponent = r.get_exponent();
    return *this;
  }

  template <typename LeftT>
  friend auto operator/(const LeftT& l, const p2_t<value_type>& r) {
    if constexpr (std::is_same_v<LeftT, p2_t<value_type>>) {
      const auto result_exp = l.get_exponent() - r.get_exponent();
      return p2_t<value_type>::from_exponent(result_exp);
    } else {
      static_assert(std::is_unsigned_v<LeftT>);
      return l >> r.get_exponent();
    }
  }

  template <typename LeftT>
  friend auto operator%(const LeftT& l, const p2_t<value_type>& r) {
    return l & (r.get_value() - 1);
  }
};

using p2_uint64_t = p2_t<std::uint64_t>;
using p2_uint32_t = p2_t<std::uint32_t>;
using p2_uint8_t = p2_t<std::uint8_t>;

} // namespace ceph::math

namespace std {

template <typename T>
struct is_integral<ceph::math::p2_t<T>> : public std::is_integral<T> {
};

template <typename T, typename U, template <typename> class P2T>
struct common_type<T, P2T<U>> : public common_type<T, U> {
};

template <typename T>
const ceph::math::p2_t<T>& max(const ceph::math::p2_t<T>& a,
                               const ceph::math::p2_t<T>& b) {
  return a.get_exponent() < b.get_exponent() ? b : a;
}

template <typename T>
const ceph::math::p2_t<T>& min(const ceph::math::p2_t<T>& a,
                               const ceph::math::p2_t<T>& b) {
  return b.get_exponent() < a.get_exponent() ? b : a;
}

} // namespace std

#endif
