// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "common/ceph_json.h"
#include "rgw_common.h"
#include "rgw_swift.h"
#include "rgw_swift_auth.h"
#include "rgw_user.h"
#include "rgw_http_client.h"
#include "rgw_keystone.h"

#include "include/str_list.h"

#define dout_subsys ceph_subsys_rgw

static vector<string> accepted_roles;
static vector<string> accepted_admin_roles;

class RGWValidateSwiftToken : public RGWHTTPClient {
  struct rgw_swift_auth_info *info;

protected:
  RGWValidateSwiftToken()
    : RGWHTTPClient(nullptr),
      info(nullptr) {
  }
public:
  RGWValidateSwiftToken(CephContext *_cct,
                        struct rgw_swift_auth_info *_info)
    : RGWHTTPClient(_cct),
      info(_info) {
  }

  int receive_header(void *ptr, size_t len);
  int receive_data(void *ptr, size_t len) {
    return 0;
  }
  int send_data(void *ptr, size_t len) {
    return 0;
  }

  friend class RGWKeystoneTokenCache;
};

int RGWValidateSwiftToken::receive_header(void *ptr, size_t len)
{
  char line[len + 1];

  char *s = (char *)ptr, *end = (char *)ptr + len;
  char *p = line;
  ldout(cct, 10) << "read_http_header" << dendl;

  while (s != end) {
    if (*s == '\r') {
      s++;
      continue;
    }
    if (*s == '\n') {
      *p = '\0';
      ldout(cct, 10) << "os_auth:" << line << dendl;
      // TODO: fill whatever data required here
      char *l = line;
      char *tok = strsep(&l, " \t:");
      if (tok) {
        while (l && *l == ' ')
          l++;
 
        if (strcmp(tok, "HTTP") == 0) {
          ;
        } else if (strcasecmp(tok, "X-Auth-Groups") == 0) {
          info->auth_groups = l;
          char *s = strchr(l, ',');
          if (s) {
            *s = '\0';
            info->user = l;
          }
        } else if (strcasecmp(tok, "X-Auth-Ttl") == 0) {
          info->ttl = atoll(l);
        }
      }
    }
    if (s != end)
      *p++ = *s++;
  }

  return 0;
}

int RGWSwift::validate_token(RGWRados * const store,
                             const char *token,
                             rgw_swift_auth_info& auth_info)    /* out */
{
  if (g_conf->rgw_swift_auth_url.empty()) {
    return -EINVAL;
  }

  string auth_url = g_conf->rgw_swift_auth_url;

  if (auth_url[auth_url.size() - 1] != '/') {
    auth_url.append("/");
  }

  auth_url.append("token");
  char url_buf[auth_url.size() + 1 + strlen(token) + 1];
  sprintf(url_buf, "%s/%s", auth_url.c_str(), token);

  RGWValidateSwiftToken validate(cct, &auth_info);

  ldout(cct, 10) << "rgw_swift_validate_token url=" << url_buf << dendl;

  int ret = validate.process(url_buf);
  if (ret < 0) {
    return ret;
  }

  if (auth_info.user.empty()) {
    ldout(cct, 5) << "swift auth didn't authorize a user" << dendl;
    return -EPERM;
  }

  string swift_user = auth_info.user.to_str();
  ldout(cct, 10) << "swift user=" << swift_user << dendl;

  RGWUserInfo tmp_uinfo;
  ret = rgw_get_user_info_by_swift(store, swift_user, tmp_uinfo);
  if (ret < 0) {
    ldout(cct, 0) << "NOTICE: couldn't map swift user" << dendl;
    return ret;
  }

  auth_info.perm_mask = get_perm_mask(swift_user, tmp_uinfo);
  auth_info.is_admin = false;

  return 0;
}

class RGWPostHTTPData : public RGWHTTPClient {
  bufferlist *bl;
  std::string post_data;
  size_t post_data_index;
  std::string subject_token;
public:
  RGWPostHTTPData(CephContext *_cct, bufferlist *_bl) : RGWHTTPClient(_cct), bl(_bl), post_data_index(0) {}
  RGWPostHTTPData(CephContext *_cct, bufferlist *_bl, bool verify_ssl) : RGWHTTPClient(_cct), bl(_bl), post_data_index(0){
    set_verify_ssl(verify_ssl);
  }

  void set_post_data(const std::string& _post_data) {
    this->post_data = _post_data;
  }

  int send_data(void* ptr, size_t len) {
    int length_to_copy = 0;
    if (post_data_index < post_data.length()) {
      length_to_copy = min(post_data.length() - post_data_index, len);
      memcpy(ptr, post_data.data() + post_data_index, length_to_copy);
      post_data_index += length_to_copy;
    }
    return length_to_copy;
  }

  int receive_data(void *ptr, size_t len) {
    bl->append((char *)ptr, len);
    return 0;
  }

  int receive_header(void *ptr, size_t len) {
    char line[len + 1];

    char *s = (char *)ptr, *end = (char *)ptr + len;
    char *p = line;
    ldout(cct, 20) << "RGWPostHTTPData::receive_header parsing HTTP headers" << dendl;

    while (s != end) {
      if (*s == '\r') {
        s++;
        continue;
      }
      if (*s == '\n') {
        *p = '\0';
        ldout(cct, 20) << "RGWPostHTTPData::receive_header: line="
                       << line << dendl;
        // TODO: fill whatever data required here
        char *l = line;
        char *tok = strsep(&l, " \t:");
        if (tok) {
          while (l && *l == ' ') {
            l++;
          }

          if (strcasecmp(tok, "X-Subject-Token") == 0) {
            subject_token = l;
          }
        }
      }
      if (s != end) {
        *p++ = *s++;
      }
    }
    return 0;
  }

  std::string get_subject_token() {
    return subject_token;
  }
};

typedef RGWPostHTTPData RGWValidateKeystoneToken;
typedef RGWPostHTTPData RGWGetKeystoneAdminToken;
typedef RGWPostHTTPData RGWGetRevokedTokens;

int RGWSwift::get_keystone_url(CephContext * const cct,
                               std::string& url)
{
  bufferlist bl;
  RGWGetRevokedTokens req(cct, &bl, cct->_conf->rgw_keystone_verify_ssl);

  url = cct->_conf->rgw_keystone_url;
  if (url.empty()) {
    ldout(cct, 0) << "ERROR: keystone url is not configured" << dendl;
    return -EINVAL;
  }
  if (url[url.size() - 1] != '/')
    url.append("/");
  return 0;
}

int RGWSwift::get_keystone_url(std::string& url)
{
  return RGWSwift::get_keystone_url(cct, url);
}

int RGWSwift::get_keystone_admin_token(std::string& token)
{
  return RGWSwift::get_keystone_admin_token(cct, token);
}

int RGWSwift::get_keystone_admin_token(CephContext * const cct,
                                       std::string& token)
{
  std::string token_url;

  if (get_keystone_url(cct, token_url) < 0) {
    return -EINVAL;
  }

  if (!cct->_conf->rgw_keystone_admin_token.empty()) {
    token = cct->_conf->rgw_keystone_admin_token;
    return 0;
  }

  KeystoneToken t;

  /* Try cache first. */
  if (RGWKeystoneTokenCache::get_instance().find_admin(t)) {
    ldout(cct, 20) << "found cached admin token" << dendl;
    token = t.token.id;
    return 0;
  }

  bufferlist token_bl;
  RGWGetKeystoneAdminToken token_req(cct, &token_bl, cct->_conf->rgw_keystone_verify_ssl);
  token_req.append_header("Content-Type", "application/json");
  JSONFormatter jf;

  const auto keystone_version = KeystoneService::get_api_version();
  if (keystone_version == KeystoneApiVersion::VER_2) {
    KeystoneAdminTokenRequestVer2 req_serializer(cct);
    req_serializer.dump(&jf);

    std::stringstream ss;
    jf.flush(ss);
    token_req.set_post_data(ss.str());
    token_req.set_send_length(ss.str().length());
    token_url.append("v2.0/tokens");

  } else if (keystone_version == KeystoneApiVersion::VER_3) {
    KeystoneAdminTokenRequestVer3 req_serializer(cct);
    req_serializer.dump(&jf);

    std::stringstream ss;
    jf.flush(ss);
    token_req.set_post_data(ss.str());
    token_req.set_send_length(ss.str().length());
    token_url.append("v3/auth/tokens");
  } else {
    return -ENOTSUP;
  }

  const int ret = token_req.process("POST", token_url.c_str());
  if (ret < 0) {
    return ret;
  }
  if (t.parse(cct, token_req.get_subject_token(), token_bl) != 0) {
    return -EINVAL;
  }

  RGWKeystoneTokenCache::get_instance().add_admin(t);
  token = t.token.id;
  return 0;
}


int RGWSwift::check_revoked()
{
  string url;
  string token;

  bufferlist bl;
  RGWGetRevokedTokens req(cct, &bl);

  if (get_keystone_admin_token(token) < 0) {
    return -EINVAL;
  }
  if (get_keystone_url(url) < 0) {
    return -EINVAL;
  }
  req.append_header("X-Auth-Token", token);

  const auto keystone_version = KeystoneService::get_api_version();
  if (keystone_version == KeystoneApiVersion::VER_2) {
    url.append("v2.0/tokens/revoked");
  } else if (keystone_version == KeystoneApiVersion::VER_3) {
    url.append("v3/auth/tokens/OS-PKI/revoked");
  }

  req.set_send_length(0);
  int ret = req.process(url.c_str());
  if (ret < 0) {
    return ret;
  }

  bl.append((char)0); // NULL terminate for debug output

  ldout(cct, 10) << "request returned " << bl.c_str() << dendl;

  JSONParser parser;

  if (!parser.parse(bl.c_str(), bl.length())) {
    ldout(cct, 0) << "malformed json" << dendl;
    return -EINVAL;
  }

  JSONObjIter iter = parser.find_first("signed");
  if (iter.end()) {
    ldout(cct, 0) << "revoked tokens response is missing signed section" << dendl;
    return -EINVAL;
  }  

  JSONObj *signed_obj = *iter;

  string signed_str = signed_obj->get_data();

  ldout(cct, 10) << "signed=" << signed_str << dendl;

  string signed_b64;
  ret = rgw_open_cms_envelope(cct, signed_str, signed_b64);
  if (ret < 0)
    return ret;

  ldout(cct, 10) << "content=" << signed_b64 << dendl;
  
  bufferlist json;
  ret = rgw_decode_b64_cms(cct, signed_b64, json);
  if (ret < 0) {
    return ret;
  }

  ldout(cct, 10) << "ceph_decode_cms: decoded: " << json.c_str() << dendl;

  JSONParser list_parser;
  if (!list_parser.parse(json.c_str(), json.length())) {
    ldout(cct, 0) << "malformed json" << dendl;
    return -EINVAL;
  }

  JSONObjIter revoked_iter = list_parser.find_first("revoked");
  if (revoked_iter.end()) {
    ldout(cct, 0) << "no revoked section in json" << dendl;
    return -EINVAL;
  }

  JSONObj *revoked_obj = *revoked_iter;

  JSONObjIter tokens_iter = revoked_obj->find_first();
  for (; !tokens_iter.end(); ++tokens_iter) {
    JSONObj *o = *tokens_iter;

    JSONObj *token = o->find_obj("id");
    if (!token) {
      ldout(cct, 0) << "bad token in array, missing id" << dendl;
      continue;
    }

    string token_id = token->get_data();
    RGWKeystoneTokenCache::get_instance().invalidate(token_id);
  }
  
  return 0;
}

static void rgw_set_keystone_token_auth_info(const KeystoneToken& token,
                                             struct rgw_swift_auth_info * const info)
{
  info->user = token.get_project_id();
  info->display_name = token.get_project_name();
  info->status = 200;

  /* Check whether the user has an admin status. */
  bool is_admin = false;
  for (const auto admin_role : accepted_admin_roles) {
    if (token.has_role(admin_role)) {
      is_admin = true;
      break;
    }
  }
  info->is_admin = is_admin;
  info->perm_mask = RGW_PERM_FULL_CONTROL;
}

int RGWSwift::parse_keystone_token_response(const string& token,
                                            bufferlist& bl,
                                            struct rgw_swift_auth_info * const info,
                                            KeystoneToken& t)
{
  int ret = t.parse(cct, token, bl);
  if (ret < 0) {
    return ret;
  }

  bool found = false;
  for (const auto role : accepted_roles) {
    if (t.has_role(role) == true) {
      found = true;
      break;
    }
  }

  if (!found) {
    ldout(cct, 0) << "user does not hold a matching role; required roles: "
                  << g_conf->rgw_keystone_accepted_roles << dendl;
    return -EPERM;
  }

  ldout(cct, 0) << "validated token: " << t.get_project_name()
                << ":" << t.get_user_name()
                << " expires: " << t.get_expires() << dendl;

  rgw_set_keystone_token_auth_info(t, info);

  return 0;
}

int RGWSwift::load_acct_info(RGWRados * const store,
                             const string& account_name,
                             const struct rgw_swift_auth_info& info,
                             RGWUserInfo& user_info)                /* out */
{
  ldout(cct, 20) << "updating user=" << info.user << dendl; // P3 XXX
  /*
   * Normally once someone parsed the token, the tenant and user are set
   * in rgw_swift_auth_info. If .tenant is empty in it, the client has
   * authenticated with the empty legacy tenant. But when we authenticate
   * with Keystone, we have a special compatibility kludge. First, we try
   * the same tenant as the user. If that user exists, we use it. This way,
   * migrated OpenStack users can get their namespaced containers and
   * nobody's the wiser. If that fails, we look up the user in the empty
   * tenant. If neither is found, make one, and those migrating can
   * set a special configurable rgw_keystone_implicit_tenants to create
   * suitable tenantized users.
   */
  if (info.user.tenant.empty()) {
    rgw_user uid = !account_name.empty() ? account_name : info.user;

    if (rgw_get_user_info_by_uid(store, uid, user_info) < 0) {
      uid.tenant.clear();

      if (rgw_get_user_info_by_uid(store, uid, user_info) < 0) {
        ldout(cct, 0) << "NOTICE: couldn't map swift user " << uid << dendl;

        if (g_conf->rgw_keystone_implicit_tenants) {
          uid.tenant = info.user.id;
        }

        if (uid != info.user) {
          ldout(cct, 0) << "ERROR: only owner may create the account" << dendl;
          return -EPERM;
        }

        user_info.user_id = uid;
        user_info.display_name = info.display_name;
        int ret = rgw_store_user_info(store, user_info, nullptr, nullptr,
                                      real_time(), true);
        if (ret < 0) {
          ldout(cct, 0) << "ERROR: failed to store new user info: user="
                        << user_info.user_id << " ret=" << ret << dendl;
          return ret;
        }
      }
    }
  } else {
    if (rgw_get_user_info_by_uid(store, info.user, user_info) < 0) {
      ldout(cct, 0) << "NOTICE: couldn't map swift user " << info.user << dendl;

      user_info.user_id = info.user;
      user_info.display_name = info.display_name;

      int ret = rgw_store_user_info(store, user_info, NULL, NULL, real_time(), true);
      if (ret < 0) {
        ldout(cct, 0) << "ERROR: failed to store new user info: user="
                      << user_info.user_id << " ret=" << ret << dendl;
        return ret;
      }
    }
  }
  return 0;
}

int RGWSwift::load_user_info(RGWRados *const store,
                             const struct rgw_swift_auth_info& auth_info,
                             rgw_user& auth_user,               /* out */
                             uint32_t& perm_mask,               /* out */
                             bool& admin_request)               /* out */
{
  if (auth_info.status != 200) {
    return -EPERM;
  }

  auth_user = auth_info.user;
  perm_mask = auth_info.perm_mask;
  admin_request = auth_info.is_admin;

  return 0;
}

int RGWSwift::validate_keystone_token(RGWRados * const store,
                                      const string& token,
                                      struct rgw_swift_auth_info *info) /* out */
{
  KeystoneToken t;

  string token_id;
  rgw_get_token_id(token, token_id);

  ldout(cct, 20) << "token_id=" << token_id << dendl;

  /* check cache first */
  if (RGWKeystoneTokenCache::get_instance().find(token_id, t)) {
    ldout(cct, 20) << "cached token.project.id=" << t.get_project_id() << dendl;
    rgw_set_keystone_token_auth_info(t, info);
    return 0;
  }

  bufferlist bl;

  /* check if that's a self signed token that we can decode */
  if (!rgw_decode_pki_token(cct, token, bl)) {

    /* can't decode, just go to the keystone server for validation */

    RGWValidateKeystoneToken validate(cct, &bl, cct->_conf->rgw_keystone_verify_ssl);

    string url = g_conf->rgw_keystone_url;
    if (url.empty()) {
      ldout(cct, 0) << "ERROR: keystone url is not configured" << dendl;
      return -EINVAL;
    }
    if (url[url.size() - 1] != '/')
      url.append("/");
    std::string admin_token;
    if (get_keystone_admin_token(admin_token) < 0)
      return -EINVAL;
    if (get_keystone_url(url) < 0)
      return -EINVAL;

    validate.append_header("X-Auth-Token", admin_token);

    const auto keystone_version = KeystoneService::get_api_version();
    if (keystone_version == KeystoneApiVersion::VER_2) {
      url.append("v2.0/tokens/");
      url.append(token);
    }
    if (keystone_version == KeystoneApiVersion::VER_3) {
      url.append("v3/auth/tokens");
      validate.append_header("X-Subject-Token", token);
    }

    validate.set_send_length(0);

    int ret = validate.process(url.c_str());
    if (ret < 0)
      return ret;
  }

  bl.append((char)0); // NULL terminate for debug output

  ldout(cct, 20) << "received response: " << bl.c_str() << dendl;

  int ret = parse_keystone_token_response(token, bl, info, t);
  if (ret < 0)
    return ret;

  if (t.expired()) {
    ldout(cct, 0) << "got expired token: " << t.get_project_name()
                  << ":" << t.get_user_name()
                  << " expired: " << t.get_expires() << dendl;
    return -EPERM;
  }

  RGWKeystoneTokenCache::get_instance().add(token_id, t);

  return 0;
}

uint32_t RGWSwift::get_perm_mask(const string& swift_user,
                                 const RGWUserInfo &uinfo)
{
  uint32_t perm_mask = 0;

  if (swift_user.empty()) {
    string subuser;
    ssize_t pos = swift_user.find(':');
    if (pos < 0) {
      subuser = swift_user;
    } else {
      subuser = swift_user.substr(pos + 1);
    }

    auto iter = uinfo.subusers.find(subuser);
    if (iter != uinfo.subusers.end()) {
      const RGWSubUser& subuser_ = iter->second;
      perm_mask = subuser_.perm_mask;
    }
  } else {
    perm_mask = RGW_PERM_FULL_CONTROL;
  }

  return perm_mask;
}

/* Keystone */
bool RGWKeystoneAuthEngine::is_applicable() const noexcept
{
  return false == cct->_conf->rgw_keystone_url.empty();
}


KeystoneToken RGWKeystoneAuthEngine::decode_pki_token(const std::string token) const
{
  bufferlist token_body_bl;
  int ret = rgw_decode_b64_cms(cct, token, token_body_bl);
  if (ret < 0) {
    throw ret;
  } else {
    ldout(cct, 20) << "successfully decoded pki token" << dendl;
  }

  KeystoneToken token_body;
  ret = token_body.parse(cct, token, token_body_bl);
  if (ret < 0) {
    throw ret;
  }

  return token_body;
}

KeystoneToken RGWKeystoneAuthEngine::get_from_keystone(const std::string token) const
{
  bufferlist token_body_bl;
  RGWValidateKeystoneToken validate(cct, &token_body_bl,
                                    cct->_conf->rgw_keystone_verify_ssl);

  std::string url;
  if (RGWSwift::get_keystone_url(cct, url) < 0) {
    throw -EINVAL;
  }

  const auto keystone_version = KeystoneService::get_api_version();
  if (keystone_version == KeystoneApiVersion::VER_2) {
    url.append("v2.0/tokens/" + token);
  } else if (keystone_version == KeystoneApiVersion::VER_3) {
    url.append("v3/auth/tokens");
    validate.append_header("X-Subject-Token", token);
  }

  std::string admin_token;
  if (RGWSwift::get_keystone_admin_token(cct, admin_token) < 0) {
    throw -EINVAL;
  }

  validate.append_header("X-Auth-Token", admin_token);
  validate.set_send_length(0);

  int ret = validate.process(url.c_str());
  if (ret < 0) {
    throw ret;
  }
  token_body_bl.append((char)0); // NULL terminate for debug output

  ldout(cct, 20) << "received response: " << token_body_bl.c_str() << dendl;

  KeystoneToken token_body;
  ret = token_body.parse(cct, token, token_body_bl);
  if (ret < 0) {
    throw ret;
  }

  return token_body;
}

RGWRemoteAuthApplier::AuthInfo
RGWKeystoneAuthEngine::get_creds_info(const KeystoneToken& token) const noexcept
{
  /* Check whether the user has an admin status. */
  bool is_admin = false;
  for (const auto admin_role : accepted_admin_roles) {
    if (token.has_role(admin_role)) {
      is_admin = true;
      break;
    }
  }

  return {
    token.get_project_id(),
    token.get_project_name(),
    RGW_PERM_FULL_CONTROL,
    is_admin
  };
}

RGWAuthApplier::aplptr_t RGWKeystoneAuthEngine::authenticate() const
{
  KeystoneToken t;

  const auto token_id = rgw_get_token_id(token);
  ldout(cct, 20) << "token_id=" << token_id << dendl;

  /* Check cache first, */
  if (RGWKeystoneTokenCache::get_instance().find(token_id, t)) {
    ldout(cct, 20) << "cached token.project.id=" << t.get_project_id() << dendl;
    return factory.create_loader(cct, get_creds_info(t));
  }

  if (rgw_is_pki_token(token)) {
    t = decode_pki_token(token);
  } else {
    /* Can't decode, just go to the Keystone server for validation. */
    t = get_from_keystone(token);
  }

  /* Verify expiration. */
  if (t.expired()) {
    ldout(cct, 0) << "got expired token: " << t.get_project_name()
                  << ":" << t.get_user_name()
                  << " expired: " << t.get_expires() << dendl;
    return nullptr;
  }

  /* Check for necessary roles. */
  for (const auto role : accepted_roles) {
    if (t.has_role(role) == true) {
      ldout(cct, 0) << "validated token: " << t.get_project_name()
                    << ":" << t.get_user_name()
                    << " expires: " << t.get_expires() << dendl;
      RGWKeystoneTokenCache::get_instance().add(token_id, t);
      return factory.create_loader(cct, get_creds_info(t));
    }
  }

  ldout(cct, 0) << "user does not hold a matching role; required roles: "
                << g_conf->rgw_keystone_accepted_roles << dendl;

  return nullptr;
}

bool RGWSwift::verify_swift_token(RGWRados *store, req_state *s)
{
  RGWTempURLAuthApplier::Factory tempurl_fact;
  RGWTempURLAuthEngine tempurl(s, store, tempurl_fact);

  RGWSignedTokenAuthEngine rgwtk(s, s->os_auth_token);

  RGWRemoteAuthApplier::Factory creating_fact(store);
  RGWKeystoneAuthEngine keystone(s, creating_fact);
  RGWExternalTokenAuthEngine ext(s, s->os_auth_token);

  const std::vector<const RGWAuthEngine *> engines = {
    &tempurl, &rgwtk, &keystone, &ext
  };

  for (const auto engine : engines) {
    if (!engine->is_applicable()) {
      /* Engine said it isn't suitable for handling this particular
       * request. Let's try a next one. */
      continue;
    }

    try {
      const auto loader = engine->authenticate();
      if (!loader) {
        /* Access denied is acknowledged by returning a std::unique_ptr with
         * nullptr inside. */
        ldout(cct, 5) << "auth engine refused to authenicate" << dendl;
        return false;
      }

      try {
        /* Account used by a given RGWOp is decoupled from identity employed
         * in the authorization phase (RGWOp::verify_permissions). */
        loader->load_acct_info(*s->user);
        loader->load_user_info(s->auth_user, s->perm_mask, s->admin_request);

        /* This is the signle place where we pass req_state as a pointer
         * to non-const and thus its modification is allowed. In the time
         * of writing only RGWTempURLEngine needed that feature. */
        loader->modify_request_state(s);
      } catch (int err) {
        ldout(cct, 5) << "loader throwed err=" << err << dendl;
        return false;
      }
    } catch (int err) {
      ldout(cct, 5) << "auth engine throwed err=" << err << dendl;
      return false;
    }

    return true;
  }

  /* All engines refused to handle this authentication request by
   * returning RGWAuthEngine::Status::UNKKOWN. Rather rare case. */
  return false;
}

bool RGWSwift::do_verify_swift_token(RGWRados *store, req_state *s)
{
  struct rgw_swift_auth_info auth_info;

  if (strncmp(s->os_auth_token, "AUTH_rgwtk", 10) == 0) {
    if (rgw_swift_verify_signed_token(s->cct, store, s->os_auth_token,
                                      auth_info) < 0) {
      return false;
    }

    if (load_acct_info(store, s->account_name, auth_info, *(s->user)) < 0) {
      return false;
    }

    if (load_user_info(store, auth_info, s->auth_user, s->perm_mask,
                       s->admin_request) < 0) {
      return false;
    }

    return  true;
  }

  if (supports_keystone()) {
    if (validate_keystone_token(store, s->os_auth_token, &auth_info) < 0) {
      /* Authentication failed. */
      return false;
    }

    if (load_acct_info(store, s->account_name, auth_info, *(s->user)) < 0) {
      return false;
    }

    if (load_user_info(store, auth_info, s->auth_user, s->perm_mask,
                       s->admin_request) < 0) {
      return false;
    }

    return true;
  }

  if (validate_token(store, s->os_auth_token, auth_info) < 0) {
    ldout(cct, 5) << "swift auth didn't authorize a user" << dendl;
    return false;
  }

  if (load_acct_info(store, s->account_name, auth_info, *(s->user)) < 0) {
    return false;
  }

  if (load_user_info(store, auth_info, s->auth_user, s->perm_mask,
                     s->admin_request) < 0) {
    return false;
  }

  ldout(cct, 10) << "user_id=" << s->user->user_id << dendl;

  return true;
}

void RGWSwift::init()
{
  get_str_vec(cct->_conf->rgw_keystone_accepted_roles, accepted_roles);
  get_str_vec(cct->_conf->rgw_keystone_accepted_admin_roles,
              accepted_admin_roles);

  accepted_roles.insert(accepted_roles.end(), accepted_admin_roles.begin(),
                        accepted_admin_roles.end());

  if (supports_keystone()) {
    init_keystone();
  }
}


void RGWSwift::init_keystone()
{
  keystone_revoke_thread = new KeystoneRevokeThread(cct, this);
  keystone_revoke_thread->create("rgw_swift_k_rev");
}


void RGWSwift::finalize()
{
  if (supports_keystone())
    finalize_keystone();
}

void RGWSwift::finalize_keystone()
{
  down_flag.set(1);
  if (keystone_revoke_thread) {
    keystone_revoke_thread->stop();
    keystone_revoke_thread->join();
  }
  delete keystone_revoke_thread;
  keystone_revoke_thread = NULL;
}

RGWSwift *rgw_swift = NULL;

void swift_init(CephContext *cct)
{
  rgw_swift = new RGWSwift(cct);
}

void swift_finalize()
{
  delete rgw_swift;
}

bool RGWSwift::going_down()
{
  return (down_flag.read() != 0);
}

void *RGWSwift::KeystoneRevokeThread::entry() {
  do {
    dout(2) << "keystone revoke thread: start" << dendl;
    int r = swift->check_revoked();
    if (r < 0) {
      dout(0) << "ERROR: keystone revocation processing returned error r=" << r << dendl;
    }

    if (swift->going_down())
      break;

    lock.Lock();
    cond.WaitInterval(cct, lock, utime_t(cct->_conf->rgw_keystone_revocation_interval, 0));
    lock.Unlock();
  } while (!swift->going_down());

  return NULL;
}

void RGWSwift::KeystoneRevokeThread::stop()
{
  Mutex::Locker l(lock);
  cond.Signal();
}
