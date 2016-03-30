// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#ifndef CEPH_RGW_SWIFT_H
#define CEPH_RGW_SWIFT_H

#include "include/assert.h"

#include "rgw_common.h"
#include "common/Cond.h"

#include "rgw_auth.h"
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


/* TempURL: applier. */
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


/* TempURL: engine */
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


/* AUTH_rgwtk */
class RGWSignedTokenAuthEngine : public RGWTokenBasedAuthEngine {
public:
  using RGWTokenBasedAuthEngine::RGWTokenBasedAuthEngine;

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

