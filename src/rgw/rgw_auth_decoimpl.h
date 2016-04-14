// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#ifndef CEPH_RGW_AUTH_DECOIMPL_H
#define CEPH_RGW_AUTH_DECOIMPL_H

#include "rgw_auth.h"


/* Abstract decorator over any implementation of RGWAuthApplier. */
template <typename DecorateeT>
class RGWDecoratingAuthApplier : public RGWAuthApplier {
  static_assert(std::is_base_of<RGWAuthApplier, DecorateeT>::value,
                "DecorateeT must be a subclass of RGWAuthApplier");
  DecorateeT decoratee;

public:
  RGWDecoratingAuthApplier(const DecorateeT& decoratee)
    : RGWAuthApplier(decoratee.cct),
      decoratee(decoratee) {
  }

  virtual int get_perms_from_aclspec(const aclspec_t& aclspec) const override {
    return decoratee.get_perms_from_aclspec(aclspec);
  }

  virtual bool is_entitled_to(const rgw_user& uid) const override {
    return decoratee.is_entitled_to(uid);
  }

  virtual bool is_owner_of(const rgw_user& uid) const override {
    return decoratee.is_oner_of(uid);
  }

  virtual void load_acct_info(RGWUserInfo& user_info) const override {  /* out */
    return decoratee.load_acct_info(user_info);
  }

  virtual void modify_request_state(req_state * s) const override {     /* in/out */
    return decoratee.modify_request_state(s);
  }
};


/* Decorator specialization for dealing with pointers to an applier. Useful
 * for decorating the applier returned after successfull authenication. */
template <>
class RGWDecoratingAuthApplier<RGWAuthApplier::aplptr_t> : public RGWAuthApplier {
  aplptr_t decoratee;

public:
  RGWDecoratingAuthApplier(aplptr_t&& decoratee)
    : RGWAuthApplier(decoratee->cct),
      decoratee(std::move(decoratee)) {
  }

  virtual int get_perms_from_aclspec(const aclspec_t& aclspec) const override {
    return decoratee->get_perms_from_aclspec(aclspec);
  }

  virtual bool is_entitled_to(const rgw_user& uid) const override {
    return decoratee->is_entitled_to(uid);
  }

  virtual bool is_owner_of(const rgw_user& uid) const override {
    return decoratee->is_owner_of(uid);
  }

  virtual void load_acct_info(RGWUserInfo& user_info) const override {  /* out */
    return decoratee->load_acct_info(user_info);
  }

  virtual void modify_request_state(req_state * s) const override {     /* in/out */
    return decoratee->modify_request_state(s);
  }
};


template <typename T>
class RGWThirdPartyAccountAuthApplier : public RGWDecoratingAuthApplier<T> {
  /* const */RGWRados * const store;
  const rgw_user acct_user_override;
public:
  /* FIXME: comment this. */
  static const rgw_user UNKNOWN_ACCT;

  template <typename U>
  RGWThirdPartyAccountAuthApplier(U&& decoratee,
                                  RGWRados * const store,
                                  const rgw_user acct_user_override)
    : RGWDecoratingAuthApplier<T>(std::move(decoratee)),
      store(store),
      acct_user_override(acct_user_override) {
  }

  virtual void load_acct_info(RGWUserInfo& user_info) const override;   /* out */
};

/* static declaration */
template <typename T>
const rgw_user RGWThirdPartyAccountAuthApplier<T>::UNKNOWN_ACCT;

template <typename T>
void RGWThirdPartyAccountAuthApplier<T>::load_acct_info(RGWUserInfo& user_info) const
{
  if (UNKNOWN_ACCT == acct_user_override) {
    /* There is no override specified by the upper layer. This means that we'll
     * load the account owned by the authenticated identity (aka auth_user). */
    RGWDecoratingAuthApplier<T>::load_acct_info(user_info);
  } else if (RGWDecoratingAuthApplier<T>::is_owner_of(acct_user_override)) {
    /* The override has been specified but the account belongs to the authenticated
     * identity. We may safely forward the call to a next stage. */
    RGWDecoratingAuthApplier<T>::load_acct_info(user_info);
  } else {
    int ret = rgw_get_user_info_by_uid(store, acct_user_override, user_info);
    if (ret < 0) {
      /* We aren't trying to recover from ENOENT here. It's supposed that creating
       * someone else's account isn't a thing we want to support. */
      throw ret;
    }
  }
}

#endif /* CEPH_RGW_AUTH_DECOIMPL_H */
