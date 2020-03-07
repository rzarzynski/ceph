// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <array>
#include <iostream>
#include <string_view>

#include "auth/Crypto.h"
#include "include/buffer.h"
#include "gtest/gtest.h"
#include "common/ceph_crypto.h"
#include "common/ceph_context.h"
#include "global/global_context.h"

#include "msg/async/crypto_onwire_aesgcm.h"

#define dout_context g_ceph_context

class CryptoEnvironment: public ::testing::Environment {
public:
  void SetUp() override {
    ceph::crypto::init();
  }
};

template <class T>
static std::unique_ptr<T> create_crypto_handler(CephContext* const cct)
{
  using ceph::crypto::onwire::key_t;
  using ceph::crypto::onwire::nonce_t;

  const auto connection_secret = \
    "mock of crypto material for deriving key and nonce for AES GCM"sv;
  ceph_assert_always(
    connection_secret.length() >= sizeof(key_t) + sizeof(nonce_t));
  const char* secbuf = connection_secret.data();

  key_t key;
  {
    ::memcpy(key.data(), secbuf, sizeof(key));
    secbuf += sizeof(key);
  }

  nonce_t nonce;
  {
    ::memcpy(&nonce, secbuf, sizeof(nonce));
    secbuf += sizeof(nonce);
  }

  return std::make_unique<T>(cct, key, nonce);
}

static auto constexpr pt = "mock of plain text for AES GCM cipher"sv;

TEST(OnwireCrypto, AESGCMTxHandler)
{
  using ceph::crypto::onwire::AES128GCM_OnWireTxHandler;
  auto tx = create_crypto_handler<AES128GCM_OnWireTxHandler>(g_ceph_context);

  ceph::bufferlist plaintext;
  {
    auto pt = "mock of plain text for AES GCM cipher"sv;
    plaintext.push_back(ceph::buffer::copy(pt.data(), pt.length()));
  }

  tx->reset_tx_handler({ plaintext.length() });
  tx->authenticated_encrypt_update(plaintext);
  auto ciphertext = tx->authenticated_encrypt_final();
  using ceph::crypto::onwire::AESGCM_TAG_LEN;
  ASSERT_EQ(pt.size() + AESGCM_TAG_LEN, ciphertext.length());
  ASSERT_NE(0, ::memcmp(ciphertext.c_str(), pt.data(), pt.size()));

  // let's ensure the input bufferlist is untoched.
  ASSERT_TRUE(plaintext.contents_equal(pt.data(), pt.size()));
}

// FIXME: replace with recording
auto create_ciphertext() {
  using ceph::crypto::onwire::AES128GCM_OnWireTxHandler;
  auto tx = create_crypto_handler<AES128GCM_OnWireTxHandler>(g_ceph_context);

  ceph::bufferlist plaintext;
  {
    plaintext.push_back(ceph::buffer::copy(pt.data(), pt.length()));
  }

  tx->reset_tx_handler({ plaintext.length() });
  tx->authenticated_encrypt_update(plaintext);
  return tx->authenticated_encrypt_final();
}

struct aes_gcm_sample_t {
  static constexpr std::string_view plain {
    "mock of plain text for AES GCM cipher"
  };

  // AES GCM doesn't do padding. The size of ciphertext is actually the same
  // as its corresponding plaintext except the AE (authenticated encryption)
  // tag at the end.
  static constexpr std::array<unsigned char,
                              std::size(plain)> encrypted {
    0x4d, 0xa2, 0xa6, 0x1b, 0xa5, 0x2e, 0x20, 0x0d,
    0xa3, 0x3e, 0x56, 0x6f, 0x36, 0x8c, 0xf3, 0x43,
    0x1a, 0xe5, 0x81, 0x55, 0xb2, 0x31, 0x8c, 0x79,
    0xe5, 0x16, 0xae, 0xab, 0x80, 0xab, 0xd9, 0xe4,
    0x13, 0x91, 0xad, 0x44, 0x7d
  };

  static constexpr std::array<unsigned char,
                              ceph::crypto::onwire::AESGCM_TAG_LEN> tag {
    0xf4, 0x91, 0x9e, 0x37, 0x0e, 0xdc, 0xa8, 0xb2,
    0xc6, 0xeb, 0xf8, 0x03, 0xe9, 0x62, 0x42, 0xc5
  };
};

template <std::size_t N>
static ceph::bufferlist to_bl(const std::array<unsigned char, N>& arr)
{
  ceph::bufferlist bl;
  bl.push_back(ceph::buffer::copy(reinterpret_cast<const char*>(arr.data()),
                                  arr.size())); 
  return bl;
}

static ceph::bufferlist to_bl(const std::string_view& sv)
{
  ceph::bufferlist bl;
  bl.push_back(ceph::buffer::copy(sv.data(), sv.size())); 
  return bl;
}

using AES128GCM_OnWireTxHandler = \
  ceph::crypto::onwire::AES128GCM_OnWireTxHandler;
using AES128GCM_OnWireRxHandler = \
  ceph::crypto::onwire::AES128GCM_OnWireRxHandler;

struct ciphertext_generator_t {
  std::unique_ptr<AES128GCM_OnWireTxHandler> tx;

  ciphertext_generator_t()
    : tx(create_crypto_handler<AES128GCM_OnWireTxHandler>(g_ceph_context)) {
  }

  ceph::bufferlist generate_cipherchunk_with_tag() {
    auto plainchunk = to_bl(aes_gcm_sample_t::plain);
    tx->reset_tx_handler({ plainchunk.length() });
    tx->authenticated_encrypt_update(std::move(plainchunk));
    return tx->authenticated_encrypt_final();
  }
};


TEST(AESGCMRxHandler, single_chunk)
{
  // decrypt and authenticate at once – using the authenticated_decrypt_update_final
  auto rx = create_crypto_handler<AES128GCM_OnWireRxHandler>(g_ceph_context);
  rx->reset_rx_handler();

  ceph::bufferlist ciphertext_with_tag;
  {
    // claim_append() needs l-value reference.
    auto ciphertext = to_bl(aes_gcm_sample_t::encrypted);
    auto tag = to_bl(aes_gcm_sample_t::tag);
    ciphertext_with_tag.claim_append(ciphertext);
    ciphertext_with_tag.claim_append(tag);
  }

  // If tag doesn't match, exception will be thrown.
  auto plaintext = rx->authenticated_decrypt_update_final(
    std::move(ciphertext_with_tag), 16);
  ASSERT_TRUE(plaintext.contents_equal(aes_gcm_sample_t::plain.data(),
                                       aes_gcm_sample_t::plain.size()));
}

TEST(AESGCMRxHandler, mismatched_tag)
{
  auto rx = create_crypto_handler<AES128GCM_OnWireRxHandler>(g_ceph_context);
  rx->reset_rx_handler();

  ceph::bufferlist ciphertext_with_badtag;
  {
    // claim_append() needs l-value reference.
    auto ciphertext = to_bl(aes_gcm_sample_t::encrypted);
    ceph::bufferlist tag;
    tag.append_zero(ceph::crypto::onwire::AESGCM_TAG_LEN);
    ciphertext_with_badtag.claim_append(ciphertext);
    ciphertext_with_badtag.claim_append(tag);
  }

  // If tag doesn't match, exception will be thrown.
  ASSERT_THROW(
    rx->authenticated_decrypt_update_final(
      std::move(ciphertext_with_badtag), 16),
    ceph::crypto::onwire::MsgAuthError);
}

TEST(AESGCMRxHandler, multi_chunk)
{
  // verify whether the ciphertext matches plaintext over the entire
  // space of chunk sizes. by chunk we understood the fragment passed
  // to authenticated_decrypt_update() – the auth tag in this test is
  // provided separately.
  for (std::size_t chunk_size = 1;
       chunk_size <= std::size(aes_gcm_sample_t::encrypted);
       chunk_size++) {
    auto rx = create_crypto_handler<AES128GCM_OnWireRxHandler>(g_ceph_context);

    rx->reset_rx_handler();

    ceph::bufferlist plaintext;
    ceph::bufferlist ciphertext = to_bl(aes_gcm_sample_t::encrypted);
    while (ciphertext.length() >= chunk_size) {
      ceph::bufferlist cipherchunk;
      ciphertext.splice(0, chunk_size, &cipherchunk);

      ceph::bufferlist plainchunk = rx->authenticated_decrypt_update(
        std::move(cipherchunk), 16);
      plaintext.claim_append(plainchunk);
    }

    if (ciphertext.length() > 0) {
      ceph::bufferlist last_plainchunk = rx->authenticated_decrypt_update(
        std::move(ciphertext), 16);
      plaintext.claim_append(last_plainchunk);
    }

    // if tag doesn't match, exception will be thrown.
    auto final_plainchunk = \
      rx->authenticated_decrypt_update_final(to_bl(aes_gcm_sample_t::tag), 16);
    ASSERT_EQ(0, final_plainchunk.length());
    ASSERT_TRUE(plaintext.contents_equal(aes_gcm_sample_t::plain.data(),
                                         aes_gcm_sample_t::plain.size()));
  }
}

TEST(AESGCMRxHandler, reset)
{
  ciphertext_generator_t ctg;
  auto rx = create_crypto_handler<AES128GCM_OnWireRxHandler>(g_ceph_context);

  for (std::size_t i = 0; i < 5; i++) {
    rx->reset_rx_handler();

    auto ciphertext_with_tag = ctg.generate_cipherchunk_with_tag();

    // If tag doesn't match, exception will be thrown.
    ceph::bufferlist plaintext;
    EXPECT_NO_THROW({
      plaintext = rx->authenticated_decrypt_update_final(
        std::move(ciphertext_with_tag), 16);
    });
    ASSERT_TRUE(plaintext.contents_equal(aes_gcm_sample_t::plain.data(),
                                         aes_gcm_sample_t::plain.size()));
  }
}

TEST(AESGCMRxHandler, reset_with_multiple_chunks)
{
  ciphertext_generator_t ctg;
  auto rx = create_crypto_handler<AES128GCM_OnWireRxHandler>(g_ceph_context);

  for (std::size_t i = 0; i < 5; i++) {
    rx->reset_rx_handler();

    ceph::bufferlist ciphertext;
    ceph::bufferlist tag;
    {
      auto ciphertext_with_tag = ctg.generate_cipherchunk_with_tag();
      const std::size_t ciphertext_size = \
        ciphertext_with_tag.length() - ceph::crypto::onwire::AESGCM_TAG_LEN;
      ciphertext_with_tag.splice(0, ciphertext_size, &ciphertext);

      // ciphertext has been moved out; the remaning is tag.
      tag = std::move(ciphertext_with_tag);
    }

    ceph::bufferlist plaintext;
    EXPECT_NO_THROW({
      plaintext = rx->authenticated_decrypt_update(
        std::move(ciphertext), 16);
    });
    ASSERT_TRUE(plaintext.contents_equal(aes_gcm_sample_t::plain.data(),
                                         aes_gcm_sample_t::plain.size()));

    // If tag doesn't match, exception will be thrown.
    ceph::bufferlist final_plaintext;
    EXPECT_NO_THROW({
      final_plaintext = rx->authenticated_decrypt_update_final(
        std::move(tag), 16);
    });
  }
}
