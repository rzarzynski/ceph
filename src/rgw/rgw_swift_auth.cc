// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_swift_auth.h"
#include "rgw_rest.h"

#include "common/ceph_crypto.h"
#include "common/Clock.h"

#include "auth/Crypto.h"

#include "rgw_client_io.h"
#include "rgw_swift.h"

#define dout_subsys ceph_subsys_rgw

#define DEFAULT_SWIFT_PREFIX "swift"

using namespace ceph::crypto;


/* TempURL: applier */
void RGWTempURLAuthApplier::modify_request_state(req_state * s) const       /* in/out */
{
  bool inline_exists = false;
  string filename = s->info.args.get("filename");

  s->info.args.get("inline", &inline_exists);
  if (inline_exists) {
    s->content_disp.override = "inline";
  } else if (!filename.empty()) {
    string fenc;
    url_encode(filename, fenc);
    s->content_disp.override = "attachment; filename=\"" + fenc + "\"";
  } else {
    string fenc;
    url_encode(s->object.name, fenc);
    s->content_disp.fallback = "attachment; filename=\"" + fenc + "\"";
  }

  ldout(s->cct, 20) << "finished applying changes to req_state for TempURL: "
                    << " content_disp override " << s->content_disp.override
                    << " content_disp fallback " << s->content_disp.fallback
                    << dendl;

}

/* TempURL: engine */
bool RGWTempURLAuthEngine::is_applicable() const noexcept
{
  return s->info.args.exists("temp_url_sig") ||
         s->info.args.exists("temp_url_expires");
}

void RGWTempURLAuthEngine::get_owner_info(RGWUserInfo& owner_info) const
{
  /* We cannot use req_state::bucket_name because it isn't available
   * now. It will be initialized in RGWHandler_REST_SWIFT::postauth_init(). */
  const string& bucket_name = s->init_state.url_bucket;

  /* TempURL requires bucket and object specified in the requets. */
  if (bucket_name.empty() || s->object.empty()) {
    throw -EPERM;
  }

  /* TempURL case is completely different than the Keystone auth - you may
   * get account name only through extraction from URL. In turn, knowledge
   * about account is neccessary to obtain its bucket tenant. Without that,
   * the access would be limited to accounts with empty tenant. */
  string bucket_tenant;
  if (!s->account_name.empty()) {
    RGWUserInfo uinfo;

    if (rgw_get_user_info_by_uid(store, s->account_name, uinfo) < 0) {
      throw -EPERM;
    } else {
      bucket_tenant = uinfo.user_id.tenant;
    }
  }

  /* Need to get user info of bucket owner. */
  RGWBucketInfo bucket_info;
  int ret = store->get_bucket_info(*static_cast<RGWObjectCtx *>(s->obj_ctx),
                                   bucket_tenant, bucket_name,
                                   bucket_info, nullptr);
  if (ret < 0) {
    throw ret;
  }

  ldout(s->cct, 20) << "temp url user (bucket owner): " << bucket_info.owner
                    << dendl;

  if (rgw_get_user_info_by_uid(store, bucket_info.owner, owner_info) < 0) {
    throw -EPERM;
  }
}

bool RGWTempURLAuthEngine::is_expired(const std::string& expires) const
{
  string err;
  const utime_t now = ceph_clock_now(g_ceph_context);
  const uint64_t expiration = (uint64_t)strict_strtoll(expires.c_str(),
                                                       10, &err);
  if (!err.empty()) {
    dout(5) << "failed to parse temp_url_expires: " << err << dendl;
    return true;
  }

  if (expiration <= (uint64_t)now.sec()) {
    dout(5) << "temp url expired: " << expiration << " <= " << now.sec() << dendl;
    return true;
  }

  return false;
}

std::string RGWTempURLAuthEngine::generate_signature(const string& key,
                                                     const string& method,
                                                     const string& path,
                                                     const string& expires) const
{
  const string str = method + "\n" + expires + "\n" + path;
  ldout(cct, 20) << "temp url signature (plain text): " << str << dendl;

  /* unsigned */ char dest[CEPH_CRYPTO_HMACSHA1_DIGESTSIZE];
  calc_hmac_sha1(key.c_str(), key.size(),
                 str.c_str(), str.size(),
                 dest);

  char dest_str[CEPH_CRYPTO_HMACSHA1_DIGESTSIZE * 2 + 1];
  buf_to_hex((const unsigned char *)dest, sizeof(dest), dest_str);

  return dest_str;
}

RGWAuthApplier::aplptr_t RGWTempURLAuthEngine::authenticate() const
{
  const string temp_url_sig = s->info.args.get("temp_url_sig");
  const string temp_url_expires = s->info.args.get("temp_url_expires");
  if (temp_url_sig.empty() || temp_url_expires.empty()) {
    return nullptr;
  }

  RGWUserInfo owner_info;
  try {
    get_owner_info(owner_info);
  } catch (...) {
    return nullptr;
  }

  if (owner_info.temp_url_keys.empty()) {
    ldout(cct, 5) << "user does not have temp url key set, aborting" << dendl;
    return nullptr;
  }

  if (is_expired(temp_url_expires)) {
    return nullptr;
  }

  /* We need to verify two paths because of compliance with Swift, Tempest
   * and old versions of RadosGW. The second item will have the prefix
   * of Swift API entry point removed. */
  const size_t pos = g_conf->rgw_swift_url_prefix.find_last_not_of('/') + 1;
  const std::vector<std::string> allowed_paths = {
    s->info.request_uri,
    s->info.request_uri.substr(pos + 1)
  };

  /* Account owner calculates the signature also against a HTTP method. */
  std::vector<std::string> allowed_methods;
  if (strcmp("HEAD", s->info.method) == 0) {
    /* HEAD requests are specially handled. */
    allowed_methods = { "HEAD", "GET", "PUT" };
  } else if (!s->info.method) {
    allowed_methods = { s->info.method };
  }

  /* Need to try each combination of keys, allowed path and methods. */
  for (const auto kv : owner_info.temp_url_keys) {
    const int temp_url_key_num = kv.first;
    const string& temp_url_key = kv.second;

    if (temp_url_key.empty()) {
      continue;
    }

    for (const auto path : allowed_paths) {
      for (const auto method : allowed_methods) {
        const std::string local_sig = generate_signature(temp_url_key,
                                                         method, path,
                                                         temp_url_expires);

        ldout(s->cct, 20) << "temp url signature [" << temp_url_key_num
                          << "] (calculated): " << local_sig
                          << dendl;

        if (local_sig != temp_url_sig) {
          ldout(s->cct,  5) << "temp url signature mismatch: " << local_sig
                            << " != " << temp_url_sig  << dendl;
        } else {
          return ldr_factory.create_loader(cct, owner_info);
        }
      }
    }
  }

  return nullptr;
}


/* AUTH_rgwtk (signed token): engine */
bool RGWSignedTokenAuthEngine::is_applicable() const noexcept
{
  return !strncmp(s->os_auth_token, "AUTH_rgwtk", 10);
}

RGWAuthApplier::aplptr_t RGWSignedTokenAuthEngine::authenticate() const
{
  return nullptr;
}


/* External token */
bool RGWExternalTokenAuthEngine::is_applicable() const noexcept
{
  return false;
}

RGWAuthApplier::aplptr_t RGWExternalTokenAuthEngine::authenticate() const
{
  return nullptr;
}


static int build_token(string& swift_user, string& key, uint64_t nonce,
		       utime_t& expiration, bufferlist& bl)
{
  ::encode(swift_user, bl);
  ::encode(nonce, bl);
  ::encode(expiration, bl);

  bufferptr p(CEPH_CRYPTO_HMACSHA1_DIGESTSIZE);

  char buf[bl.length() * 2 + 1];
  buf_to_hex((const unsigned char *)bl.c_str(), bl.length(), buf);
  dout(20) << "build_token token=" << buf << dendl;

  char k[CEPH_CRYPTO_HMACSHA1_DIGESTSIZE];
  memset(k, 0, sizeof(k));
  const char *s = key.c_str();
  for (int i = 0; i < (int)key.length(); i++, s++) {
    k[i % CEPH_CRYPTO_HMACSHA1_DIGESTSIZE] |= *s;
  }
  calc_hmac_sha1(k, sizeof(k), bl.c_str(), bl.length(), p.c_str());

  bl.append(p);

  return 0;

}

static int encode_token(CephContext *cct, string& swift_user, string& key,
			bufferlist& bl)
{
  uint64_t nonce;

  int ret = get_random_bytes((char *)&nonce, sizeof(nonce));
  if (ret < 0)
    return ret;

  utime_t expiration = ceph_clock_now(cct);
  expiration += cct->_conf->rgw_swift_token_expiration;

  ret = build_token(swift_user, key, nonce, expiration, bl);

  return ret;
}

int rgw_swift_verify_signed_token(CephContext * const cct,
                                  RGWRados * const store,
                                  const char *token,
                                  rgw_swift_auth_info& auth_info)
{
  if (strncmp(token, "AUTH_rgwtk", 10) != 0)
    return -EINVAL;

  token += 10;

  int len = strlen(token);
  if (len & 1) {
    dout(0) << "NOTICE: failed to verify token: invalid token length len="
	    << len << dendl;
    return -EINVAL;
  }

  bufferptr p(len/2);
  int ret = hex_to_buf(token, p.c_str(), len);
  if (ret < 0)
    return ret;

  bufferlist bl;
  bl.append(p);

  bufferlist::iterator iter = bl.begin();

  uint64_t nonce;
  utime_t expiration;
  string swift_user;

  try {
    ::decode(swift_user, iter);
    ::decode(nonce, iter);
    ::decode(expiration, iter);
  } catch (buffer::error& err) {
    dout(0) << "NOTICE: failed to decode token: caught exception" << dendl;
    return -EINVAL;
  }
  utime_t now = ceph_clock_now(cct);
  if (expiration < now) {
    dout(0) << "NOTICE: old timed out token was used now=" << now
	    << " token.expiration=" << expiration << dendl;
    return -EPERM;
  }

  RGWUserInfo info;
  if ((ret = rgw_get_user_info_by_swift(store, swift_user, info)) < 0)
    return ret;

  dout(10) << "swift_user=" << swift_user << dendl;

  map<string, RGWAccessKey>::iterator siter = info.swift_keys.find(swift_user);
  if (siter == info.swift_keys.end())
    return -EPERM;
  RGWAccessKey& swift_key = siter->second;

  bufferlist tok;
  ret = build_token(swift_user, swift_key.key, nonce, expiration, tok);
  if (ret < 0)
    return ret;

  if (tok.length() != bl.length()) {
    dout(0) << "NOTICE: tokens length mismatch: bl.length()=" << bl.length()
	    << " tok.length()=" << tok.length() << dendl;
    return -EPERM;
  }

  if (memcmp(tok.c_str(), bl.c_str(), tok.length()) != 0) {
    char buf[tok.length() * 2 + 1];
    buf_to_hex((const unsigned char *)tok.c_str(), tok.length(), buf);
    dout(0) << "NOTICE: tokens mismatch tok=" << buf << dendl;
    return -EPERM;
  }

  auth_info.user = info.user_id;
  auth_info.is_admin = info.admin;
  auth_info.perm_mask = RGWSwift::get_perm_mask(swift_user, info);
  auth_info.status = 200;

  return 0;
}

void RGW_SWIFT_Auth_Get::execute()
{
  int ret = -EPERM;

  const char *key = s->info.env->get("HTTP_X_AUTH_KEY");
  const char *user = s->info.env->get("HTTP_X_AUTH_USER");

  string user_str;
  RGWUserInfo info;
  bufferlist bl;
  RGWAccessKey *swift_key;
  map<string, RGWAccessKey>::iterator siter;

  string swift_url = g_conf->rgw_swift_url;
  string swift_prefix = g_conf->rgw_swift_url_prefix;
  string tenant_path;

  if (swift_prefix.size() == 0) {
    swift_prefix = DEFAULT_SWIFT_PREFIX;
  }

  if (swift_url.size() == 0) {
    bool add_port = false;
    const char *server_port = s->info.env->get("SERVER_PORT_SECURE");
    const char *protocol;
    if (server_port) {
      add_port = (strcmp(server_port, "443") != 0);
      protocol = "https";
    } else {
      server_port = s->info.env->get("SERVER_PORT");
      add_port = (strcmp(server_port, "80") != 0);
      protocol = "http";
    }
    const char *host = s->info.env->get("HTTP_HOST");
    if (!host) {
      dout(0) << "NOTICE: server is misconfigured, missing rgw_swift_url_prefix or rgw_swift_url, HTTP_HOST is not set" << dendl;
      ret = -EINVAL;
      goto done;
    }
    swift_url = protocol;
    swift_url.append("://");
    swift_url.append(host);
    if (add_port && !strchr(host, ':')) {
      swift_url.append(":");
      swift_url.append(server_port);
    }
  }

  if (!key || !user)
    goto done;

  user_str = user;

  if ((ret = rgw_get_user_info_by_swift(store, user_str, info)) < 0)
  {
    ret = -EACCES;
    goto done;
  }

  siter = info.swift_keys.find(user_str);
  if (siter == info.swift_keys.end()) {
    ret = -EPERM;
    goto done;
  }
  swift_key = &siter->second;

  if (swift_key->key.compare(key) != 0) {
    dout(0) << "NOTICE: RGW_SWIFT_Auth_Get::execute(): bad swift key" << dendl;
    ret = -EPERM;
    goto done;
  }

  if (!g_conf->rgw_swift_tenant_name.empty()) {
    tenant_path = "/AUTH_";
    tenant_path.append(g_conf->rgw_swift_tenant_name);
  } else if (g_conf->rgw_swift_account_in_url) {
    tenant_path = "/AUTH_";
    tenant_path.append(user_str);
  }

  STREAM_IO(s)->print("X-Storage-Url: %s/%s/v1%s\r\n", swift_url.c_str(),
		swift_prefix.c_str(), tenant_path.c_str());

  if ((ret = encode_token(s->cct, swift_key->id, swift_key->key, bl)) < 0)
    goto done;

  {
    char buf[bl.length() * 2 + 1];
    buf_to_hex((const unsigned char *)bl.c_str(), bl.length(), buf);

    STREAM_IO(s)->print("X-Storage-Token: AUTH_rgwtk%s\r\n", buf);
    STREAM_IO(s)->print("X-Auth-Token: AUTH_rgwtk%s\r\n", buf);
  }

  ret = STATUS_NO_CONTENT;

done:
  set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s);
}

int RGWHandler_SWIFT_Auth::init(RGWRados *store, struct req_state *state,
				RGWClientIO *cio)
{
  state->dialect = "swift-auth";
  state->formatter = new JSONFormatter;
  state->format = RGW_FORMAT_JSON;

  return RGWHandler::init(store, state, cio);
}

int RGWHandler_SWIFT_Auth::authorize()
{
  return 0;
}

RGWOp *RGWHandler_SWIFT_Auth::op_get()
{
  return new RGW_SWIFT_Auth_Get;
}

