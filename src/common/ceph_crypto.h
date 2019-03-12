// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
#ifndef CEPH_CRYPTO_H
#define CEPH_CRYPTO_H

#include "acconfig.h"
#include <stdexcept>

#include "include/buffer.h"
#include "include/types.h"

#define CEPH_CRYPTO_MD5_DIGESTSIZE 16
#define CEPH_CRYPTO_HMACSHA1_DIGESTSIZE 20
#define CEPH_CRYPTO_SHA1_DIGESTSIZE 20
#define CEPH_CRYPTO_HMACSHA256_DIGESTSIZE 32
#define CEPH_CRYPTO_SHA256_DIGESTSIZE 32
#define CEPH_CRYPTO_SHA512_DIGESTSIZE 64

#ifdef USE_NSS
// you *must* use CRYPTO_CXXFLAGS in CMakeLists.txt for including this include
# include <nss.h>
# include <pk11pub.h>

// NSS thinks a lot of fairly fundamental operations might potentially
// fail, because it has been written to support e.g. smartcards doing all
// the crypto operations. We don't want to contaminate too much code
// with error checking, and just say these really should never fail.
// This assert MUST NOT be compiled out, even on non-debug builds.
# include "include/ceph_assert.h"
#endif /*USE_NSS*/

#ifdef USE_OPENSSL
#include <openssl/evp.h>
#include <openssl/ossl_typ.h>
#include <openssl/hmac.h>

extern "C" {
  const EVP_MD *EVP_md5(void);
  const EVP_MD *EVP_sha1(void);
  const EVP_MD *EVP_sha256(void);
  const EVP_MD *EVP_sha512(void);
}
#endif /*USE_OPENSSL*/

namespace ceph {
  namespace crypto {
    // workaround for no PK11_ImportSymKey in FIPS mode
    PK11SymKey *PK11_ImportSymKey_FIPS(
	PK11SlotInfo *slot,
	CK_MECHANISM_TYPE type,
	PK11Origin origin,
	CK_ATTRIBUTE_TYPE operation,
	SECItem *key,
	void *wincx);
  } // namespace crypto
} // namespace


namespace ceph {
  namespace crypto {
    void assert_init();
    void init(CephContext *cct);
    void shutdown(bool shared=true);
  }
}

#ifdef USE_NSS
namespace ceph {
  namespace crypto {

    class DigestException : public std::runtime_error
    {
    public:
      DigestException(const char* what_arg) : runtime_error(what_arg)
	{}
    };

    namespace nss {

      template<size_t DigestSize>
      class NSSDigest {
      private:
        PK11Context *ctx;
      public:
        static constexpr size_t digest_size = DigestSize;
        NSSDigest (SECOidTag _type) {
	  ctx = PK11_CreateDigestContext(_type);
	  if (! ctx) {
	    throw DigestException("PK11_CreateDigestContext() failed");
	  }
	  Restart();
        }
        ~NSSDigest () {
	  PK11_DestroyContext(ctx, PR_TRUE);
	}
	void Restart() {
	  SECStatus s;
	  s = PK11_DigestBegin(ctx);
	  if (s != SECSuccess) {
	    throw DigestException("PK11_DigestBegin() failed");
	  }
	}
	void Update (const unsigned char *input, size_t length) {
	  if (length) {
	    SECStatus s;
	    s = PK11_DigestOp(ctx, input, length);
	    if (s != SECSuccess) {
	      throw DigestException("PK11_DigestOp() failed");
	    }
	  }
	}
	void Final (unsigned char *digest) {
	  SECStatus s;
	  unsigned int dummy;
	  s = PK11_DigestFinal(ctx, digest, &dummy, digest_size);
	  if (! (s == SECSuccess) &&
	      (dummy == digest_size)) {
	    throw DigestException("PK11_DigestFinal() failed");
	  }
	  Restart();
	}
      };

      class MD5 : public NSSDigest<CEPH_CRYPTO_MD5_DIGESTSIZE> {
      public:
	MD5 () : NSSDigest<CEPH_CRYPTO_MD5_DIGESTSIZE>{SEC_OID_MD5} { }
      };

      class SHA1 : public NSSDigest<CEPH_CRYPTO_SHA1_DIGESTSIZE> {
      public:
        SHA1 () : NSSDigest<CEPH_CRYPTO_SHA1_DIGESTSIZE>{SEC_OID_SHA1} { }
      };

      class SHA256 : public NSSDigest<CEPH_CRYPTO_SHA256_DIGESTSIZE> {
      public:
        SHA256 () : NSSDigest<CEPH_CRYPTO_SHA256_DIGESTSIZE>{SEC_OID_SHA256} { }
      };

      class SHA512 : public NSSDigest<CEPH_CRYPTO_SHA256_DIGESTSIZE> {
      public:
        SHA512 () : NSSDigest<CEPH_CRYPTO_SHA256_DIGESTSIZE>{SEC_OID_SHA512} { }
      };
    }
  }
}
#endif /*USE_NSS*/

#ifdef USE_OPENSSL
namespace ceph {
  namespace crypto {
    namespace ssl {
      class OpenSSLDigest {
      private:
	EVP_MD_CTX *mpContext;
	const EVP_MD *mpType;
      public:
	OpenSSLDigest (const EVP_MD *_type);
	~OpenSSLDigest ();
	void Restart();
	void Update (const unsigned char *input, size_t length);
	void Final (unsigned char *digest);
      };

      class MD5 : public OpenSSLDigest {
      public:
	static constexpr size_t digest_size = CEPH_CRYPTO_MD5_DIGESTSIZE;
	MD5 () : OpenSSLDigest(EVP_md5()) { }
      };

      class SHA1 : public OpenSSLDigest {
      public:
        static constexpr size_t digest_size = CEPH_CRYPTO_SHA1_DIGESTSIZE;
        SHA1 () : OpenSSLDigest(EVP_sha1()) { }
      };

      class SHA256 : public OpenSSLDigest {
      public:
        static constexpr size_t digest_size = CEPH_CRYPTO_SHA256_DIGESTSIZE;
        SHA256 () : OpenSSLDigest(EVP_sha256()) { }
      };

      class SHA512 : public OpenSSLDigest {
      public:
        static constexpr size_t digest_size = CEPH_CRYPTO_SHA512_DIGESTSIZE;
        SHA512 () : OpenSSLDigest(EVP_sha512()) { }
      };
    }
  }
}
#endif /*USE_OPENSSL*/


#ifdef USE_NSS
namespace ceph {
  namespace crypto::nss {
    class HMAC {
    private:
      PK11SlotInfo *slot;
      PK11SymKey *symkey;
      PK11Context *ctx;
      unsigned int digest_size;
    public:
      HMAC (CK_MECHANISM_TYPE cktype, unsigned int digestsize, const unsigned char *key, size_t length) {
        digest_size = digestsize;
	slot = PK11_GetBestSlot(cktype, NULL);
	if (! slot) {
	  throw DigestException("PK11_GetBestSlot() failed");
	}
	SECItem keyItem;
	keyItem.type = siBuffer;
	keyItem.data = (unsigned char*)key;
	keyItem.len = length;
	symkey = PK11_ImportSymKey_FIPS(slot, cktype, PK11_OriginUnwrap,
					CKA_SIGN,  &keyItem, NULL);
	if (! symkey) {
	  throw DigestException("PK11_ImportSymKey() failed");
	}
	SECItem param;
	param.type = siBuffer;
	param.data = NULL;
	param.len = 0;
	ctx = PK11_CreateContextBySymKey(cktype, CKA_SIGN, symkey, &param);
	if (! ctx) {
	  throw DigestException("PK11_CreateContextBySymKey() failed");
	}
	Restart();
      }
      ~HMAC ();
      void Restart() {
	SECStatus s;
	s = PK11_DigestBegin(ctx);
	if (s != SECSuccess) {
	  throw DigestException("PK11_DigestBegin() failed");
	}
      }
      void Update (const unsigned char *input, size_t length) {
	SECStatus s;
	s = PK11_DigestOp(ctx, input, length);
	if (s != SECSuccess) {
	  throw DigestException("PK11_DigestOp() failed");
	}
      }
      void Final (unsigned char *digest) {
	SECStatus s;
	unsigned int dummy;
	s = PK11_DigestFinal(ctx, digest, &dummy, digest_size);
	if (! (s == SECSuccess) &&
	    (dummy == digest_size)) {
	  throw DigestException("PK11_DigestFinal() failed");
	}
	Restart();
      }
    };

    class HMACSHA1 : public HMAC {
    public:
      HMACSHA1 (const unsigned char *key, size_t length) : HMAC(CKM_SHA_1_HMAC, CEPH_CRYPTO_HMACSHA1_DIGESTSIZE, key, length) { }
    };

    class HMACSHA256 : public HMAC {
    public:
      HMACSHA256 (const unsigned char *key, size_t length) : HMAC(CKM_SHA256_HMAC, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, key, length) { }
    };
  }
}
#endif

#ifdef USE_OPENSSL
namespace ceph::crypto::ssl {
# if OPENSSL_VERSION_NUMBER < 0x10100000L
  class HMAC {
  private:
    HMAC_CTX mContext;
    const EVP_MD *mpType;

  public:
    HMAC (const EVP_MD *type, const unsigned char *key, size_t length)
      : mpType(type) {
      ::memset(&mContext, 0, sizeof(mContext));
      const auto r = HMAC_Init_ex(&mContext, key, length, mpType, nullptr);
      if (r != 1) {
	  throw DigestException("HMAC_Init_ex() failed");
      }
    }
    ~HMAC () {
      HMAC_CTX_cleanup(&mContext);
    }

    void Restart () {
      const auto r = HMAC_Init_ex(&mContext, nullptr, 0, mpType, nullptr);
      if (r != 1) {
	throw DigestException("HMAC_Init_ex() failed");
      }
    }
    void Update (const unsigned char *input, size_t length) {
      if (length) {
        const auto r = HMAC_Update(&mContext, input, length);
	if (r != 1) {
	  throw DigestException("HMAC_Update() failed");
	}
      }
    }
    void Final (unsigned char *digest) {
      unsigned int s;
      const auto r = HMAC_Final(&mContext, digest, &s);
      if (r != 1) {
	throw DigestException("HMAC_Final() failed");
      }
    }
  };
# else
  class HMAC {
  private:
    HMAC_CTX *mpContext;

  public:
    HMAC (const EVP_MD *type, const unsigned char *key, size_t length)
      : mpContext(HMAC_CTX_new()) {
      const auto r = HMAC_Init_ex(mpContext, key, length, type, nullptr);
      if (r != 1) {
	throw DigestException("HMAC_Init_ex() failed");
      }
    }
    ~HMAC () {
      HMAC_CTX_free(mpContext);
    }

    void Restart () {
      const EVP_MD * const type = HMAC_CTX_get_md(mpContext);
      const auto r = HMAC_Init_ex(mpContext, nullptr, 0, type, nullptr);
      if (r != 1) {
	throw DigestException("HMAC_Init_ex() failed");
      }
    }
    void Update (const unsigned char *input, size_t length) {
      if (length) {
        const auto r = HMAC_Update(mpContext, input, length);
	if (r != 1) {
	  throw DigestException("HMAC_Update() failed");
	}
      }
    }
    void Final (unsigned char *digest) {
      unsigned int s;
      const auto r = HMAC_Final(mpContext, digest, &s);
      if (r != 1) {
	throw DigestException("HMAC_Final() failed");
      }
    }
  };
# endif // OPENSSL_VERSION_NUMBER < 0x10100000L

  struct HMACSHA1 : public HMAC {
    HMACSHA1 (const unsigned char *key, size_t length)
      : HMAC(EVP_sha1(), key, length) {
    }
  };

  struct HMACSHA256 : public HMAC {
    HMACSHA256 (const unsigned char *key, size_t length)
      : HMAC(EVP_sha256(), key, length) {
    }
  };
}
#endif /*USE_OPENSSL*/


#if defined(USE_OPENSSL)
namespace ceph {
  namespace crypto {
    using ceph::crypto::ssl::SHA256;
    using ceph::crypto::ssl::MD5;
    using ceph::crypto::ssl::SHA1;
    using ceph::crypto::ssl::SHA512;

    using ceph::crypto::ssl::HMACSHA256;
    using ceph::crypto::ssl::HMACSHA1;
  }
}
#elif defined(USE_NSS)
namespace ceph {
  namespace crypto {
    using ceph::crypto::nss::SHA256;
    using ceph::crypto::nss::MD5;
    using ceph::crypto::nss::SHA1;
    using ceph::crypto::nss::SHA512;

    using ceph::crypto::nss::HMACSHA256;
    using ceph::crypto::nss::HMACSHA1;
  }
}
#else
// cppcheck-suppress preprocessorErrorDirective
# error "No supported crypto implementation found."
#endif

namespace ceph::crypto {
template<class Digest>
auto digest(const ceph::buffer::list& bl)
{
  unsigned char fingerprint[Digest::digest_size];
  Digest gen;
  for (auto& p : bl.buffers()) {
    gen.Update((const unsigned char *)p.c_str(), p.length());
  }
  gen.Final(fingerprint);
  return sha_digest_t<Digest::digest_size>{fingerprint};
}
}

#endif
