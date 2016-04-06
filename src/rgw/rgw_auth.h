// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#ifndef CEPH_RGW_AUTH_H
#define CEPH_RGW_AUTH_H

#include <type_traits>

#include "rgw_common.h"
#include "rgw_keystone.h"

#define RGW_USER_ANON_ID "anonymous"


/* Load information about identity that will be used by RGWOp to authorize
 * any operation that comes from an authenticated user. */
class RGWIdentityApplier {
public:
  typedef std::map<std::string, int> aclspec_t;

  virtual ~RGWIdentityApplier() {};

  /* Translate the ACL provided in @aclspec into concrete permission set that
   * can be used in authorization phase (particularly in verify_permission
   * method of a given RGWOp).
   *
   * XXX: implementation is responsible for giving the real semantic to the
   * items in @aclspec. That is, their meaning may depend on particular auth
   * engine that was used. */
  virtual int get_perms_from_aclspec(const aclspec_t& aclspec) const = 0;

  /* Verify whether a given identity *can be treated as* an admin of
   * the rgw_user (account in Swift's terminology) specified in @uid. */
  virtual bool is_admin_of(const rgw_user& uid) const = 0;

  /* Verify whether a given identity *is* the owner of the rgw_user
  * (account in Swift's terminology) specified in @uid. */
  virtual bool is_owner_of(const rgw_user& uid) const = 0;

  /* Return the permission mask that is used to narrow down the set of
   * operations allowed for a given identity. This method reflects the idea
   * of subuser tied to RGWUserInfo. */
  virtual int get_perm_mask() const = 0;

  virtual bool is_anonymous() const final {
    /* If the identity owns the anonymous account (rgw_user), it's considered
     * the anonymous identity. */
    return is_owner_of(rgw_user(RGW_USER_ANON_ID));
  }
};

inline ostream& operator<<(ostream& out, const RGWIdentityApplier &id) {
//  return out << id.to_str();
  return out;
}

std::unique_ptr<RGWIdentityApplier>
rgw_auth_transform_old_authinfo(req_state * const s);


/* Interface for classes applying changes to request state/RADOS store imposed
 * by a particular RGWAuthEngine.
 *
 * Implementations must also conform to RGWIdentityApplier interface to apply
 * authorization policy (ACLs, account's ownership and entitlement).
 *
 * In contrast to RGWAuthEngine, implementations of this interface are allowed
 * to handle req_state or RGWRados in the read-write manner. */
class RGWAuthApplier : public RGWIdentityApplier {
  template <typename DecorateeT>
   friend class RGWDecoratingAuthApplier;
protected:
  CephContext * const cct;

  typedef rgw_user identity_t;

public:
  typedef std::unique_ptr<RGWAuthApplier> aplptr_t;

  RGWAuthApplier(CephContext * const cct) : cct(cct) {}
  virtual ~RGWAuthApplier() {};

  /* Fill provided RGWUserInfo with information about the account that
   * RGWOp will operate on. Errors are handled solely through exceptions.
   *
   * XXX: be aware that the "account" term refers to rgw_user. The naming
   * is legacy. */
  virtual void load_acct_info(RGWUserInfo& user_info) const = 0; /* out */

  /* Apply any changes to request state. This method will be most useful for
   * TempURL of Swift API or AWSv4. */
  virtual void modify_request_state(req_state * s) const {}      /* in/out */
};


/* RGWRemoteAuthApplier targets those authentication engines which don't need
 * to ask the RADOS store while performing the auth process. Instead, they
 * obtain credentials from an external source like Keystone or LDAP.
 *
 * As the authenticated user may not have an account yet, RGWRemoteAuthApplier
 * must be able to create it basing on data passed by an auth engine. Those
 * data will be used to fill RGWUserInfo structure. */
class RGWRemoteAuthApplier : public RGWAuthApplier {
public:
  class AuthInfo {
    friend class RGWRemoteAuthApplier;
  protected:
    const rgw_user acct_user;
    const std::string acct_name;
    const std::vector<identity_t> identities;
    const uint32_t perm_mask;
    const bool is_admin;

  public:
    AuthInfo(const rgw_user acct_user,
             const std::string acct_name,
             const std::vector<identity_t> identities,
             const uint32_t perm_mask,
             const bool is_admin)
    : acct_user(acct_user),
      acct_name(acct_name),
      identities(identities),
      perm_mask(perm_mask),
      is_admin(is_admin) {
    }
  };

protected:
  /* Read-write is intensional here due to RGWUserInfo creation process. */
  RGWRados * const store;
  const AuthInfo info;

  RGWRemoteAuthApplier(CephContext * const cct,
                       RGWRados * const store,
                       const AuthInfo info)
    : RGWAuthApplier(cct),
      store(store),
      info(info) {
  }

  virtual void create_account(const rgw_user acct_user,
                              RGWUserInfo& user_info) const;          /* out */

public:
  virtual int get_perms_from_aclspec(const aclspec_t& aclspec) const override;
  virtual bool is_admin_of(const rgw_user& uid) const override;
  virtual bool is_owner_of(const rgw_user& uid) const override;
  virtual int get_perm_mask() const { return info.perm_mask; }
  virtual void load_acct_info(RGWUserInfo& user_info) const override; /* out */

  class Factory;
};

class RGWRemoteAuthApplier::Factory {
protected:
  RGWRados * const store;

public:
  Factory(RGWRados * const store) : store(store) {}
  virtual ~Factory() {}

  virtual aplptr_t create_loader(CephContext * const cct,
                                 const AuthInfo info) const {
    return aplptr_t(new RGWRemoteAuthApplier(cct, store, info));
  }
};


/* Local auth applier targets those auth engines that store user information
 * in the RADOS store. As a consequence of performing the authentication, they
 * will have the RGWUserInfo structure loaded. Exploiting this trait allows to
 * avoid additional call to underlying RADOS store. */
class RGWLocalAuthApplier : public RGWAuthApplier {
protected:
  const RGWUserInfo user_info;
  const std::string subuser;

  RGWLocalAuthApplier(CephContext * const cct,
                      const RGWUserInfo& user_info,
                      const std::string subuser)
    : RGWAuthApplier(cct),
      user_info(user_info),
      subuser(subuser) {
  }

  uint32_t get_perm_mask(const std::string& subuser_name,
                         const RGWUserInfo &uinfo) const;

public:
  static const std::string NO_SUBUSER;

  virtual int get_perms_from_aclspec(const aclspec_t& aclspec) const override;
  virtual bool is_admin_of(const rgw_user& uid) const override;
  virtual bool is_owner_of(const rgw_user& uid) const override;
  virtual int get_perm_mask() const { return get_perm_mask(subuser, user_info); }
  virtual void load_acct_info(RGWUserInfo& user_info) const;     /* out */

  class Factory;
};

class RGWLocalAuthApplier::Factory {
public:
  virtual ~Factory() {}

  virtual aplptr_t create_loader(CephContext * const cct,
                                 const RGWUserInfo& user_info,
                                 const std::string& subuser) const {
    return aplptr_t(new RGWLocalAuthApplier(cct, user_info, subuser));
  }
};


/* Abstract class for authentication backends (auth engines) in RadosGW.
 *
 * An engine is supposed only to:
 *  - authenticate (not authorize!) a given request basing on req_state,
 *  - provide an upper layer with RGWAuthApplier to commit all changes to
 *    data structures (like req_state) and to the RADOS store (creating
 *    an account, synchronizing user personal info).
 *    Auth engine MUST NOT make any changes to req_state nor RADOS store.
 *
 * Separation between authentication and global state modification has been
 * introduced because many auth engines are perfectly orthogonal to applier
 * and thus they can be decoupled. Additional motivation is clearly distinguish
 * all places which can modify underlying data structures. */
class RGWAuthEngine {
protected:
  CephContext * const cct;

  RGWAuthEngine(CephContext * const cct)
    : cct(cct) {
  }
  /* Make the engines non-copyable and non-moveable due to const-correctness
   * and aggregating applier factories less costly and error-prone. */
  RGWAuthEngine(const RGWAuthEngine&) = delete;
  RGWAuthEngine& operator=(const RGWAuthEngine&) = delete;

public:
  /* Fast, non-throwing method for screening whether a concrete engine may
   * be interested in handling a specific request. */
  virtual bool is_applicable() const noexcept = 0;

  /* Throwing method for identity verification. When the check is positive
   * an implementation should return RGWAuthApplier::aplptr_t containing
   * a non-null pointer to object conforming the RGWAuthApplier interface.
   * Otherwise, the authentication is treated as failed.
   * An error may be signalised by throwing an exception of int type with
   * errno value inside. Those value are always negative. */
  virtual RGWAuthApplier::aplptr_t authenticate() const = 0;

  virtual ~RGWAuthEngine() {};
};


/* Abstract base class for all token-based auth engines. */
class RGWTokenBasedAuthEngine : public RGWAuthEngine {
protected:
  const std::string token;

public:
  RGWTokenBasedAuthEngine(CephContext * const cct,
                          const std::string token)
    : RGWAuthEngine(cct),
      token(token) {
  }

  bool is_applicable() const noexcept override {
    return !token.empty();
  }
};

/* TODO: introduce extractors for TokenBased. */

/* Keystone. */
class RGWKeystoneAuthEngine : public RGWTokenBasedAuthEngine {
protected:
  const RGWRemoteAuthApplier::Factory * const apl_factory;

  /* Helper methods. */
  KeystoneToken decode_pki_token(const std::string token) const;
  KeystoneToken get_from_keystone(const std::string token) const;
  RGWRemoteAuthApplier::AuthInfo get_creds_info(const KeystoneToken& token,
                                                const std::vector<std::string>& admin_roles
                                               ) const noexcept;
public:
  RGWKeystoneAuthEngine(CephContext * const cct,
                        const std::string token,
                        const RGWRemoteAuthApplier::Factory * const apl_factory)
    : RGWTokenBasedAuthEngine(cct, token),
      apl_factory(apl_factory) {
  }

  bool is_applicable() const noexcept override;
  RGWAuthApplier::aplptr_t authenticate() const override;
};


/* Anonymous */
class RGWAnonymousAuthEngine : public RGWAuthEngine {
  const RGWLocalAuthApplier::Factory * const apl_factory;

public:
  RGWAnonymousAuthEngine(CephContext * const cct,
                         const RGWLocalAuthApplier::Factory * const apl_factory)
    : RGWAuthEngine(cct),
      apl_factory(apl_factory) {
  }

  bool is_applicable() const noexcept override {
    return true;
  }

  RGWAuthApplier::aplptr_t authenticate() const override;
};

#endif /* CEPH_RGW_AUTH_H */
