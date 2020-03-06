// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_CRYPTO_ONWIRE_AESGCM_H
#define CEPH_CRYPTO_ONWIRE_AESGCM_H

#include <array>

#include "crypto_onwire.h"

namespace ceph::crypto::onwire {

static constexpr const std::size_t AESGCM_KEY_LEN{16};
static constexpr const std::size_t AESGCM_IV_LEN{12};
static constexpr const std::size_t AESGCM_TAG_LEN{16};
static constexpr const std::size_t AESGCM_BLOCK_LEN{16};

struct nonce_t {
  std::uint32_t random_seq;
  std::uint64_t random_rest;
} __attribute__((packed));
static_assert(sizeof(nonce_t) == AESGCM_IV_LEN);

using key_t = std::array<std::uint8_t, AESGCM_KEY_LEN>;

// http://www.mindspring.com/~dmcgrew/gcm-nist-6.pdf
// https://www.openssl.org/docs/man1.0.2/crypto/EVP_aes_128_gcm.html#GCM-mode
// https://wiki.openssl.org/index.php/EVP_Authenticated_Encryption_and_Decryption
// https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38d.pdf
class AES128GCM_OnWireTxHandler : public ceph::crypto::onwire::TxHandler {
  CephContext* const cct;
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> ectx;
  ceph::bufferlist buffer;
  nonce_t nonce;
  static_assert(sizeof(nonce) == AESGCM_IV_LEN);

public:
  AES128GCM_OnWireTxHandler(CephContext* const cct,
			    const key_t& key,
			    const nonce_t& nonce);
  ~AES128GCM_OnWireTxHandler() override;

  std::uint32_t calculate_segment_size(std::uint32_t size) override
  {
    return size;
  }

  void reset_tx_handler(
    std::initializer_list<std::uint32_t> update_size_sequence) override;

  void authenticated_encrypt_update(const ceph::bufferlist& plaintext) override;
  ceph::bufferlist authenticated_encrypt_final() override;
};


// RX PART
class AES128GCM_OnWireRxHandler : public ceph::crypto::onwire::RxHandler {
  CephContext* const cct;
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> ectx;
  nonce_t nonce;
  static_assert(sizeof(nonce) == AESGCM_IV_LEN);

public:
  AES128GCM_OnWireRxHandler(CephContext* const cct,
			    const key_t& key,
			    const nonce_t& nonce);
  ~AES128GCM_OnWireRxHandler() override;

  std::uint32_t get_extra_size_at_final() override {
    return AESGCM_TAG_LEN;
  }
  void reset_rx_handler() override;
  ceph::bufferlist authenticated_decrypt_update(
    ceph::bufferlist&& ciphertext,
    std::uint32_t alignment) override;
  ceph::bufferlist authenticated_decrypt_update_final(
    ceph::bufferlist&& ciphertext,
    std::uint32_t alignment) override;
};

} // namespace ceph::crypto::onwire

#endif // CEPH_CRYPTO_ONWIRE_AESGCM_H
