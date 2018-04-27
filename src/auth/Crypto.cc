// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2009 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <sstream>
#include "Crypto.h"
#ifdef USE_OPENSSL
# include <openssl/aes.h>
#endif

#include "include/assert.h"
#include "common/Clock.h"
#include "common/armor.h"
#include "common/ceph_crypto.h"
#include "common/hex.h"
#include "common/safe_io.h"
#include "include/ceph_fs.h"
#include "include/compat.h"
#include "common/Formatter.h"
#include "common/debug.h"
#include <errno.h>

// use getentropy() if available. it uses the same source of randomness
// as /dev/urandom without the filesystem overhead
#ifdef HAVE_GETENTROPY

#include <unistd.h>

CryptoRandom::CryptoRandom() : fd(0) {}
CryptoRandom::~CryptoRandom() = default;

void CryptoRandom::get_bytes(char *buf, int len)
{
  auto ret = TEMP_FAILURE_RETRY(::getentropy(buf, len));
  if (ret < 0) {
    throw std::system_error(errno, std::system_category());
  }
}

#else // !HAVE_GETENTROPY

// open /dev/urandom once on construction and reuse the fd for all reads
CryptoRandom::CryptoRandom()
  : fd(TEMP_FAILURE_RETRY(::open("/dev/urandom", O_RDONLY)))
{
  if (fd < 0) {
    throw std::system_error(errno, std::system_category());
  }
}

CryptoRandom::~CryptoRandom()
{
  VOID_TEMP_FAILURE_RETRY(::close(fd));
}

void CryptoRandom::get_bytes(char *buf, int len)
{
  auto ret = safe_read_exact(fd, buf, len);
  if (ret < 0) {
    throw std::system_error(-ret, std::system_category());
  }
}

#endif


// ---------------------------------------------------

class CryptoNoneKeyHandler : public CryptoKeyHandler {
public:
  int encrypt(const bufferlist& in,
	       bufferlist& out, std::string *error) const override {
    out = in;
    return 0;
  }
  int decrypt(const bufferlist& in,
	      bufferlist& out, std::string *error) const override {
    out = in;
    return 0;
  }

  std::size_t encrypt(const in_slice_t& in,
		      const out_slice_t& out) const override {
    // TODO: memcpy
    return 0;
  }
  std::size_t decrypt(const in_slice_t& in,
		      const out_slice_t& out) const override {
    // TODO: memcpy
    return 0;
  }
};

class CryptoNone : public CryptoHandler {
public:
  CryptoNone() { }
  ~CryptoNone() override {}
  int get_type() const override {
    return CEPH_CRYPTO_NONE;
  }
  int create(CryptoRandom *random, bufferptr& secret) override {
    return 0;
  }
  int validate_secret(const bufferptr& secret) override {
    return 0;
  }
  CryptoKeyHandler *get_key_handler(const bufferptr& secret, string& error) override {
    return new CryptoNoneKeyHandler;
  }
};


// ---------------------------------------------------


class CryptoAES : public CryptoHandler {
public:
  CryptoAES() { }
  ~CryptoAES() override {}
  int get_type() const override {
    return CEPH_CRYPTO_AES;
  }
  int create(CryptoRandom *random, bufferptr& secret) override;
  int validate_secret(const bufferptr& secret) override;
  CryptoKeyHandler *get_key_handler(const bufferptr& secret, string& error) override;
};

#ifdef USE_OPENSSL
// when we say AES, we mean AES-128
# define AES_KEY_LEN	16
# define AES_BLOCK_LEN   16

class CryptoAESKeyHandler : public CryptoKeyHandler {
  AES_KEY enc_key;
  AES_KEY dec_key;

public:
  int init(const bufferptr& s, ostringstream& err) {
    secret = s;

    const int enc_key_ret = \
      AES_set_encrypt_key((const unsigned char*)secret.c_str(), 128, &enc_key);
    if (enc_key_ret != 0) {
      err << "cannot set OpenSSL encrypt key for AES: " << enc_key_ret;
      return -1;
    }

    const int dec_key_ret = \
      AES_set_decrypt_key((const unsigned char*)secret.c_str(), 128, &dec_key);
    if (dec_key_ret != 0) {
      err << "cannot set OpenSSL decrypt key for AES: " << dec_key_ret;
      return -1;
    }

    return 0;
  }

  int encrypt(const ceph::bufferlist& in,
	      ceph::bufferlist& out,
              std::string* /* unused */) const override {
    // we need to take into account the PKCS#7 padding. There *always* will
    // be at least one byte of padding. This stays even to input aligned to
    // AES_BLOCK_LEN. Otherwise we would face ambiguities during decryption.
    ceph::bufferptr out_tmp{
      AES_BLOCK_LEN + p2align(in.length(), AES_BLOCK_LEN)};

    // let's pad the data
    std::uint8_t pad_len = out_tmp.length() - in.length();
    ceph::bufferptr pad_buf{pad_len};
    memset(pad_buf.c_str(), pad_len, pad_len);

    // form contiguous buffer for block cipher. The ctor copies shallowly.
    ceph::bufferlist incopy(in);
    incopy.append(std::move(pad_buf));
    const auto in_buf = reinterpret_cast<unsigned char*>(incopy.c_str());

    // reinitialize IV each time. It might be unnecessary depending on
    // actual implementation but at the interface layer we are obliged
    // to deliver IV as non-const.
    static_assert(strlen_ct(CEPH_AES_IV) == AES_BLOCK_LEN);
    unsigned char iv[sizeof(CEPH_AES_IV)];
    memcpy(iv, CEPH_AES_IV, sizeof(CEPH_AES_IV));

    // we aren't using EVP because of performance concerns. Profiling
    // shows the cost is quite high. Endianness might be an issue.
    // However, as they would affect Cephx, any fallout should pop up
    // rather early, hopefully.
    AES_cbc_encrypt(in_buf, reinterpret_cast<unsigned char*>(out_tmp.c_str()),
		    out_tmp.length(), &enc_key, iv, AES_ENCRYPT);

    out.append(out_tmp);
    return 0;
  }

  int decrypt(const ceph::bufferlist& in,
	      ceph::bufferlist& out,
	      std::string* /* unused */) const override {
    ceph::bufferptr out_tmp{
      AES_BLOCK_LEN + p2align(in.length(), AES_BLOCK_LEN)};

    // needed because of .c_str() on const. It's a shallow copy.
    ceph::bufferlist incopy(in);
    const auto in_buf = reinterpret_cast<unsigned char*>(incopy.c_str());

    // make a local, modifiable copy of IV.
    static_assert(strlen_ct(CEPH_AES_IV) == AES_BLOCK_LEN);
    unsigned char iv[sizeof(CEPH_AES_IV)];
    memcpy(iv, CEPH_AES_IV, sizeof(CEPH_AES_IV));

    AES_cbc_encrypt(in_buf, reinterpret_cast<unsigned char*>(out_tmp.c_str()),
		    out_tmp.length(), &dec_key, iv, AES_DECRYPT);

    // BE CAREFUL: we cannot expose any single bit of information about
    // the cause of failure. Otherwise we'll face padding oracle attack.
    // See: https://en.wikipedia.org/wiki/Padding_oracle_attack.
    if (in.length() > 1) {
      const std::uint8_t pad_len = out_tmp[in.length() - 1];
      out_tmp.set_length(in.length() - pad_len);
      out.append(std::move(out_tmp));
    }

    return 0;
  }

  std::size_t encrypt(const in_slice_t& in,
		      const out_slice_t& out) const override {
    return 0;
  }
  std::size_t decrypt(const in_slice_t& in,
		      const out_slice_t& out) const override {
    return 0;
  }
};

#else
# error "No supported crypto implementation found."
#endif



// ------------------------------------------------------------

int CryptoAES::create(CryptoRandom *random, bufferptr& secret)
{
  bufferptr buf(AES_KEY_LEN);
  random->get_bytes(buf.c_str(), buf.length());
  secret = std::move(buf);
  return 0;
}

int CryptoAES::validate_secret(const bufferptr& secret)
{
  if (secret.length() < (size_t)AES_KEY_LEN) {
    return -EINVAL;
  }

  return 0;
}

CryptoKeyHandler *CryptoAES::get_key_handler(const bufferptr& secret,
					     string& error)
{
  CryptoAESKeyHandler *ckh = new CryptoAESKeyHandler;
  ostringstream oss;
  if (ckh->init(secret, oss) < 0) {
    error = oss.str();
    delete ckh;
    return NULL;
  }
  return ckh;
}




// --


// ---------------------------------------------------


void CryptoKey::encode(bufferlist& bl) const
{
  using ceph::encode;
  encode(type, bl);
  encode(created, bl);
  __u16 len = secret.length();
  encode(len, bl);
  bl.append(secret);
}

void CryptoKey::decode(bufferlist::iterator& bl)
{
  using ceph::decode;
  decode(type, bl);
  decode(created, bl);
  __u16 len;
  decode(len, bl);
  bufferptr tmp;
  bl.copy_deep(len, tmp);
  if (_set_secret(type, tmp) < 0)
    throw buffer::malformed_input("malformed secret");
}

int CryptoKey::set_secret(int type, const bufferptr& s, utime_t c)
{
  int r = _set_secret(type, s);
  if (r < 0)
    return r;
  this->created = c;
  return 0;
}

int CryptoKey::_set_secret(int t, const bufferptr& s)
{
  if (s.length() == 0) {
    secret = s;
    ckh.reset();
    return 0;
  }

  CryptoHandler *ch = CryptoHandler::create(t);
  if (ch) {
    int ret = ch->validate_secret(s);
    if (ret < 0) {
      delete ch;
      return ret;
    }
    string error;
    ckh.reset(ch->get_key_handler(s, error));
    delete ch;
    if (error.length()) {
      return -EIO;
    }
  } else {
      return -EOPNOTSUPP;
  }
  type = t;
  secret = s;
  return 0;
}

int CryptoKey::create(CephContext *cct, int t)
{
  CryptoHandler *ch = CryptoHandler::create(t);
  if (!ch) {
    if (cct)
      lderr(cct) << "ERROR: cct->get_crypto_handler(type=" << t << ") returned NULL" << dendl;
    return -EOPNOTSUPP;
  }
  bufferptr s;
  int r = ch->create(cct->random(), s);
  delete ch;
  if (r < 0)
    return r;

  r = _set_secret(t, s);
  if (r < 0)
    return r;
  created = ceph_clock_now();
  return r;
}

void CryptoKey::print(std::ostream &out) const
{
  out << encode_base64();
}

void CryptoKey::to_str(std::string& s) const
{
  int len = secret.length() * 4;
  char buf[len];
  hex2str(secret.c_str(), secret.length(), buf, len);
  s = buf;
}

void CryptoKey::encode_formatted(string label, Formatter *f, bufferlist &bl)
{
  f->open_object_section(label.c_str());
  f->dump_string("key", encode_base64());
  f->close_section();
  f->flush(bl);
}

void CryptoKey::encode_plaintext(bufferlist &bl)
{
  bl.append(encode_base64());
}


// ------------------

CryptoHandler *CryptoHandler::create(int type)
{
  switch (type) {
  case CEPH_CRYPTO_NONE:
    return new CryptoNone;
  case CEPH_CRYPTO_AES:
    return new CryptoAES;
  default:
    return NULL;
  }
}
