// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2010-2011 Dreamhost
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/ceph_context.h"
#include "common/config.h"
#include "ceph_crypto.h"

#include <openssl/evp.h>

#ifndef OPENSSL_API_1_1
# include <openssl/conf.h>
# include <openssl/err.h>
#endif /* not OPENSSL_API_1_1 */

namespace ceph::crypto::ssl {

#ifndef OPENSSL_API_1_1
static std::atomic_uint32_t crypto_refs;

// XXX: vector instead?
static pthread_mutex_t* ssl_mutexes;
static size_t ssl_num_locks;
#endif /* not OPENSSL_API_1_1 */

#ifndef OPENSSL_API_1_1
static void
ssl_locking_callback(int mode, int mutex_num, const char *file, int line)
{
  static_cast<void>(line);
  static_cast<void>(file);

  if (mode & 1) {
    /* 1 is CRYPTO_LOCK */
    [[maybe_unused]] auto r = pthread_mutex_lock(&ssl_mutexes[mutex_num]);
  } else {
    [[maybe_unused]] auto r = pthread_mutex_unlock(&ssl_mutexes[mutex_num]);
  }
}

static unsigned long
ssl_get_thread_id(void)
{
  static_assert(sizeof(unsigned long) >= sizeof(pthread_t));
  /* pthread_t may be any data type, so a simple cast to unsigned long
   * can rise a warning/error, depending on the platform.
   * Here memcpy is used as an anything-to-anything cast. */
  unsigned long ret = 0;
  pthread_t t = pthread_self();
  memcpy(&ret, &t, sizeof(pthread_t));
  return ret;
}
#endif /* not OPENSSL_API_1_1 */

static void init() {
#ifndef OPENSSL_API_1_1
  if (++crypto_refs != 1) {
    return;
  }

  /* According to https://wiki.openssl.org/index.php/Library_Initialization#libcrypto_Initialization */
  OpenSSL_add_all_algorithms();
  ERR_load_crypto_strings();

  /* Initialize locking callbacks, needed for thread safety.
   * http://www.openssl.org/support/faq.html#PROG1
   */
  ssl_num_locks = std::max(CRYPTO_num_locks(), 0);
  if (ssl_num_locks > 0) {
    try {
      ssl_mutexes = new pthread_mutex_t[ssl_num_locks];
    } catch (...) {
      ceph_assert_always("can't allocate memory for OpenSSL init" == nullptr);
    }
  }

  pthread_mutexattr_t pthread_mutex_attr;
  pthread_mutexattr_init(&pthread_mutex_attr);
  pthread_mutexattr_settype(&pthread_mutex_attr, PTHREAD_MUTEX_RECURSIVE);

  for (size_t i = 0; i < ssl_num_locks; i++) {
    pthread_mutex_init(&ssl_mutexes[i], &pthread_mutex_attr);
  }

  CRYPTO_set_locking_callback(&ssl_locking_callback);
  CRYPTO_set_id_callback(&ssl_get_thread_id);

  OPENSSL_config(nullptr);
#endif /* OPENSSL_API_1_1 */
}

static void shutdown() {
#ifdef OPENSSL_API_1_1
  if (--crypto_refs != 0) {
    return;
  }

  /* Shutdown according to
   * https://wiki.openssl.org/index.php/Library_Initialization#Cleanup
   * http://stackoverflow.com/questions/29845527/how-to-properly-uninitialize-openssl
   */
  CRYPTO_set_locking_callback(nullptr);
  CRYPTO_set_id_callback(nullptr);
  ENGINE_cleanup();
  CONF_modules_unload(1);
  ERR_free_strings();
  EVP_cleanup();
  CRYPTO_cleanup_all_ex_data();
  ERR_remove_state(0);

  for (size_t i = 0; i < ssl_num_locks; i++) {
    pthread_mutex_destroy(&ssl_mutexes[i]);
  }
  delete[] ssl_mutexes;
  ssl_mutexes = nullptr;
#endif /* OPENSSL_API_1_1 */
}

} // namespace ceph::crypto::openssl


void ceph::crypto::init() {
  ceph::crypto::ssl::init();
}

void ceph::crypto::shutdown([[maybe_unused]] const bool shared) {
  ceph::crypto::ssl::shutdown();
}

ceph::crypto::ssl::OpenSSLDigest::OpenSSLDigest(const EVP_MD * _type)
  : mpContext(EVP_MD_CTX_create())
  , mpType(_type) {
  this->Restart();
}

ceph::crypto::ssl::OpenSSLDigest::~OpenSSLDigest() {
  EVP_MD_CTX_destroy(mpContext);
}

void ceph::crypto::ssl::OpenSSLDigest::Restart() {
  EVP_DigestInit_ex(mpContext, mpType, NULL);
}

void ceph::crypto::ssl::OpenSSLDigest::Update(const unsigned char *input, size_t length) {
  if (length) {
    EVP_DigestUpdate(mpContext, const_cast<void *>(reinterpret_cast<const void *>(input)), length);
  }
}

void ceph::crypto::ssl::OpenSSLDigest::Final(unsigned char *digest) {
  unsigned int s;
  EVP_DigestFinal_ex(mpContext, digest, &s);
}
