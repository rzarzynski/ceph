// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#ifndef CEPH_RGW_SWIFT_H
#define CEPH_RGW_SWIFT_H

#include "include/assert.h"

#include "rgw_common.h"
#include "common/Cond.h"

#include "rgw_keystone.h"

class RGWRados;

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

/* Interface for classes applying changes to request state/RADOS store imposed
 * by a particular RGWAuthEngine.
 *
 * In contrast to RGWAuthEngine, implementations of this interface are allowed
 * to handle req_state or RGWRados in the read-write manner. */
class RGWAuthApplier {
protected:
  CephContext * const cct;
public:
  typedef std::unique_ptr<RGWAuthApplier> aplptr_t;

  RGWAuthApplier(CephContext * const cct) : cct(cct) {}
  virtual ~RGWAuthApplier() {};

  /* Fill provided RGWUserInfo with information about the account that
   * RGWOp may operate on. Errors are handled solely through exceptions. */
  virtual void load_acct_info(RGWUserInfo& user_info) const = 0; /* out */

  /* Load information about identity that will be used by RGWOp to authorize
   * any operation that comes from an authenticated user. */
  virtual void load_user_info(rgw_user& auth_user,               /* out */
                              uint32_t& perm_mask,               /* out */
                              bool& admin_request) const = 0;    /* out */

  /* Apply any changes to request state. This method will be most useful for
   * TempURL of Swift API or AWSv4. */
  virtual void modify_request_state(req_state * s) const {}      /* in/out */
};


/* RGWCreatingAuthApplier - applier typical for auth engines which don't need
 * to ask the RADOS store about user credentials but instead obtain them from
 * an external source-of-truth like Keystone or LDAP.
 *
 * In such cases the applier must be able to create internal data structures
 * for users who log-in for first time. */
class RGWCreatingAuthApplier : public RGWAuthApplier {
public:
  class AuthInfo {
    friend class RGWCreatingAuthApplier;
  protected:
    const rgw_user acct_user;
    const rgw_user auth_user;
    const std::string display_name;
    const uint32_t perm_mask;
    const bool is_admin;

    static const rgw_user UNKNOWN_ACCT;

  public:
    AuthInfo(const rgw_user acct_user,
             const rgw_user auth_user,
             const std::string display_name,
             const uint32_t perm_mask,
             const bool is_admin)
    : acct_user(acct_user),
      auth_user(auth_user),
      display_name(display_name),
      perm_mask(perm_mask),
      is_admin(is_admin) {
    }

    /* Constructor for engines that aren't aware about user account. They know
     * only user's identity and its associated rights. Account will be deduced
     * for them. */
    AuthInfo(const rgw_user auth_user,
             const std::string display_name,
             const uint32_t perm_mask,
             const bool is_admin)
      : AuthInfo(rgw_user(), auth_user, display_name, perm_mask, is_admin) {
    }
  };

protected:
  /* Read-write is intensional here due to RGWUserInfo creation process.. */
  RGWRados * const store;
  const AuthInfo info;

  RGWCreatingAuthApplier(CephContext * const cct,
                         RGWRados * const store,
                         const AuthInfo info)
    : RGWAuthApplier(cct),
      store(store),
      info(info) {
  }

  virtual void create_account(const rgw_user acct_user,
                              RGWUserInfo& user_info) const;          /* out */

public:
  virtual void load_acct_info(RGWUserInfo& user_info) const override; /* out */
  virtual void load_user_info(rgw_user& auth_user,                    /* out */
                              uint32_t& perm_mask,                    /* out */
                              bool& admin_request) const override;    /* out */
  class Factory;
};

class RGWCreatingAuthApplier::Factory {
protected:
  RGWRados * const store;

public:
  Factory(RGWRados * const store)
    : store(store) {
  }
  virtual ~Factory() {}

  virtual aplptr_t create_loader(CephContext * const cct,
                                 const AuthInfo info) const {
    return aplptr_t(new RGWCreatingAuthApplier(cct, store, info));
  }
};


/* Local auth applier targets those auth engines that store user information
 * in the RADOS store. As a consequence, to perform the authentication, they
 * already have RGWUserInfo structure loaded.
 */
class RGWLocalAuthApplier : public RGWAuthApplier {
protected:
  const RGWUserInfo user_info;

  RGWLocalAuthApplier(CephContext * const cct,
                      const RGWUserInfo& user_info)
    : RGWAuthApplier(cct),
      user_info(user_info) {
  }

public:
  virtual void load_acct_info(RGWUserInfo& user_info) const;     /* out */
  virtual void load_user_info(rgw_user& auth_user,               /* out */
                              uint32_t& perm_mask,               /* out */
                              bool& admin_request) const;        /* out */
  class Factory;
};

class RGWLocalAuthApplier::Factory {
public:
  virtual ~Factory() {}

  virtual aplptr_t create_loader(CephContext * const cct,
                                 const RGWUserInfo& user_info) const {
    return aplptr_t(new RGWLocalAuthApplier(cct, user_info));
  }
};


/* TempURL loaders. */
class RGWTempURLAuthApplier : public RGWLocalAuthApplier {
protected:
  RGWTempURLAuthApplier(CephContext * const cct,
                        const RGWUserInfo& user_info)
    : RGWLocalAuthApplier(cct, user_info) {
  };

public:
  virtual void modify_request_state(req_state * s) const override; /* in/out */

  class Factory;
};

class RGWTempURLAuthApplier::Factory : public RGWLocalAuthApplier::Factory {
  friend class RGWTempURLAuthEngine;
protected:
  virtual aplptr_t create_loader(CephContext * const cct,
                                 const RGWUserInfo& user_info) const override {
    return aplptr_t(new RGWTempURLAuthApplier(cct, user_info));
  }

public:
  using RGWLocalAuthApplier::Factory::Factory;
  virtual ~Factory() {};
};

/* Abstract interface for authentication backends (auth engines) in RadosGW.
 *
 * An engine is supposed only to:
 *  - authenticate (not authorize!) a given request basing on req_state,
 *  - provide upper layer with means to fill necessary data structures
 *    in req_state through loader compliant to the RGWAuthApplier interface.
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
  /* Fast, non-throwing method for screening whether a concrete engine may
   * be interested in handling a specific request. */
  virtual bool is_applicable() const noexcept = 0;

  /* Full-blown method for identity verification. When the check is positive
   * an implementation should return std::unique_ptr containing a non-null
   * pointer. Otherwise, the authentication is treated as failed.
   * An error may be signalised by throwing an exception. */
  virtual RGWAuthApplier::aplptr_t authenticate() const = 0;

  virtual ~RGWAuthEngine() {};
};


/* TempURL */
class RGWTempURLAuthEngine : public RGWAuthEngine {
protected:
  /* const */ RGWRados * const store;
  const RGWTempURLAuthApplier::Factory& ldr_factory;

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
                       const RGWTempURLAuthApplier::Factory ldr_factory)
    : RGWAuthEngine(s),
      store(store),
      ldr_factory(ldr_factory) {
  }

  /* Interface implementations. */
  bool is_applicable() const noexcept override;
  RGWAuthApplier::aplptr_t authenticate() const override;
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
  RGWAuthApplier::aplptr_t authenticate() const override;
};


/* Keystone */
class RGWKeystoneAuthEngine : public RGWAuthEngine {
protected:
  const std::string& token;
  const RGWCreatingAuthApplier::Factory& factory;

  /* Helper methods. */
  KeystoneToken decode_pki_token(const std::string token) const;
  KeystoneToken get_from_keystone(const std::string token) const;
  RGWCreatingAuthApplier::AuthInfo get_creds_info(const KeystoneToken& token) const noexcept;
public:
  RGWKeystoneAuthEngine(const req_state * const s,
                        const RGWCreatingAuthApplier::Factory& factory)
    : RGWAuthEngine(s),
      token(s->os_auth_token),
      factory(factory) {
  }

  bool is_applicable() const noexcept override;
  RGWAuthApplier::aplptr_t authenticate() const override;
};


/* External token */
class RGWExternalTokenAuthEngine : public RGWTokenBasedAuthEngine {
public:
  using RGWTokenBasedAuthEngine::RGWTokenBasedAuthEngine;

  bool is_applicable() const noexcept override;
  RGWAuthApplier::aplptr_t authenticate() const override;
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

