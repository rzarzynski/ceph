// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#ifndef CEPH_RGW_SWIFT_H
#define CEPH_RGW_SWIFT_H

#include "include/assert.h"

#include "rgw_common.h"
#include "common/Cond.h"

class RGWRados;
class KeystoneToken;

struct rgw_swift_auth_info {
  int status;
  string auth_groups;
  rgw_user user;
  string display_name;
  long long ttl;
  bool is_admin;
  uint32_t perm_mask;

  rgw_swift_auth_info()
    : status(401), /* start with access denied */
      ttl(0),
      is_admin(false),
      perm_mask(0) {
  }
};

class RGWAuthLoader {
public:
  virtual ~RGWAuthLoader() {};

  /* Fill provided RGWUserInfo with information about the account that
   * RGWOp may operate on. Errors are handled solely through exceptions. */
  virtual void load_acct_info(RGWUserInfo& user_info) const = 0; /* out */

  /* Load information about identity that will be used by RGWOp to authorize
   * any operation that comes from authenticated user. */
  virtual void load_user_info(rgw_user& auth_user,               /* out */
                              uint32_t& perm_mask,               /* out */
                              bool& admin_request) const = 0;    /* out */

  virtual void modify_request_state(req_state * s) const {}      /* in/out */
};

class RGWCreatingAuthLoader : public RGWAuthLoader {
protected:
  using RGWAuthLoader::RGWAuthLoader;
};

class RGWNonCreatingAuthLoader : public RGWAuthLoader {
protected:
  using RGWAuthLoader::RGWAuthLoader;
};

/* TempURL loaders. */
class RGWTempURLAuthLoader : public RGWNonCreatingAuthLoader {
protected:
  const RGWUserInfo user_info;
  RGWTempURLAuthLoader(const RGWUserInfo& user_info)
    : user_info(user_info) {
  };

public:
  virtual void load_acct_info(RGWUserInfo& user_info) const;     /* out */
  virtual void load_user_info(rgw_user& auth_user,               /* out */
                              uint32_t& perm_mask,               /* out */
                              bool& admin_request) const;        /* out */
  virtual void modify_request_state(req_state * s) const;        /* in/out */

  class Factory;
};

class RGWTempURLAuthLoader::Factory {
  friend class RGWTempURLAuthEngine;

protected:
  virtual std::unique_ptr<RGWAuthLoader> create_loader(const RGWUserInfo& user_info) const {
    return std::unique_ptr<RGWAuthLoader>(new RGWTempURLAuthLoader(user_info));
  };

public:
  virtual ~Factory() {};
};

/* Abstract interface for authentication backends (auth engines) in RadosGW.
 *
 * An engine is supposed only to:
 *  - authenticate (not authorize!) a given request basing on req_state,
 *  - provide upper layer with means to fill necessary data structures
 *    in req_state through loader compliant to the RGWAuthLoader interface.
 *    Auth engine MUST NOT make any changes to req_state nor RADOS store.
 *
 * Separation between authentication and data loading has been introduced
 * because many auth engines are perfectly orthogonal to loaders and thus
 * they can be decoupled. Additional motivation is clearly distinguish
 * all places which can modify underlying structures. */
class RGWAuthEngine {
protected:
  const req_state * const s;
  CephContext * const cct;

  RGWAuthEngine(const req_state * const s)
    : s(s),
      cct(s->cct) {
  }

public:
  typedef std::unique_ptr<RGWAuthLoader> ldr_t;

  /* Fast, non-throwing method for screening whether a concrete engine may
   * be interested in handling a specific request. */
  virtual bool is_applicable() const noexcept = 0;

  /* Full-blown method for identity verification. When the check is positive
   * an implementation should return std::unique_ptr containing a non-null
   * pointer. Otherwise, the authentication is treated as failed.
   * An error may be signalised by throwing an exception. */
  virtual std::unique_ptr<RGWAuthLoader> authenticate() const = 0;

  virtual ~RGWAuthEngine() {};
};


/* TempURL */
class RGWTempURLAuthEngine : public RGWAuthEngine {
protected:
  /* const */ RGWRados * const store;
  const RGWTempURLAuthLoader::Factory& ldr_factory;

  /* Helper methods. */
  void get_owner_info(RGWUserInfo& owner_info) const;
  bool is_expired(const std::string& expires) const;
  std::string generate_signature(const string& key,
                                 const string& method,
                                 const string& path,
                                 const string& expires) const;
public:
  RGWTempURLAuthEngine(const req_state * const s,
                       /*const*/ RGWRados * const store,
                       const RGWTempURLAuthLoader::Factory ldr_factory)
    : RGWAuthEngine(s),
      store(store),
      ldr_factory(ldr_factory) {
  }

  /* Interface implementations. */
  bool is_applicable() const noexcept override;
  std::unique_ptr<RGWAuthLoader> authenticate() const override;
};

/* Virtual class for all token-based auth engines. */
class RGWTokenBasedAuthEngine : public RGWAuthEngine {
protected:
  const std::string token;

public:
  RGWTokenBasedAuthEngine(const req_state * const s,
                          const std::string token)
    : RGWAuthEngine(s),
      token(token) {
  }
};

/* AUTH_rgwtk */
class RGWSignedTokenAuthEngine : public RGWTokenBasedAuthEngine {
public:
  using RGWTokenBasedAuthEngine::RGWTokenBasedAuthEngine;

  bool is_applicable() const noexcept override;
  std::unique_ptr<RGWAuthLoader> authenticate() const override;
};

/* Keystone */
class RGWKeystoneAuthEngine : public RGWTokenBasedAuthEngine {
public:
  using RGWTokenBasedAuthEngine::RGWTokenBasedAuthEngine;

  bool is_applicable() const noexcept override;
  std::unique_ptr<RGWAuthLoader> authenticate() const override;
};

/* External token */
class RGWExternalTokenAuthEngine : public RGWTokenBasedAuthEngine {
public:
  using RGWTokenBasedAuthEngine::RGWTokenBasedAuthEngine;

  bool is_applicable() const noexcept override;
  std::unique_ptr<RGWAuthLoader> authenticate() const override;
};


class RGWSwift {
  CephContext *cct;
  atomic_t down_flag;

  int validate_token(RGWRados * store,
                     const char *token,
                     rgw_swift_auth_info& info);
  int validate_keystone_token(RGWRados *store,
                              const string& token,
                              struct rgw_swift_auth_info *info);

  int parse_keystone_token_response(const string& token,
                                    bufferlist& bl,
                                    struct rgw_swift_auth_info *info,
		                    KeystoneToken& t);
  int load_acct_info(RGWRados *store,
                     const string& account_name,        /* in */
                     const rgw_swift_auth_info& info,   /* in */
                     RGWUserInfo& user_info);           /* out */
  int load_user_info(RGWRados *store,
                     const rgw_swift_auth_info& info,   /* in */
                     rgw_user& auth_user,               /* out */
                     uint32_t& perm_mask,               /* out */
                     bool& admin_request);              /* out */

  int get_keystone_url(std::string& url);
  int get_keystone_admin_token(std::string& token);

  class KeystoneRevokeThread : public Thread {
    CephContext *cct;
    RGWSwift *swift;
    Mutex lock;
    Cond cond;

  public:
    KeystoneRevokeThread(CephContext *_cct, RGWSwift *_swift) : cct(_cct), swift(_swift), lock("KeystoneRevokeThread") {}
    void *entry();
    void stop();
  };

  KeystoneRevokeThread *keystone_revoke_thread;

  void init();
  void finalize();
  void init_keystone();
  void finalize_keystone();
  bool supports_keystone() {
    return !cct->_conf->rgw_keystone_url.empty();
  }
  bool do_verify_swift_token(RGWRados *store, req_state *s);
protected:
  int check_revoked();
public:

  explicit RGWSwift(CephContext *_cct) : cct(_cct), keystone_revoke_thread(NULL) {
    init();
  }
  ~RGWSwift() {
    finalize();
  }

  bool verify_swift_token(RGWRados *store, req_state *s);
  static uint32_t get_perm_mask(const string& swift_user,
                                const RGWUserInfo &uinfo);
  bool going_down();

  /* Static methods shared between Swift API and S3. */
  static int get_keystone_url(CephContext *cct, std::string& url);
  static int get_keystone_admin_token(CephContext *cct, std::string& token);
};

extern RGWSwift *rgw_swift;
void swift_init(CephContext *cct);
void swift_finalize();

#endif

