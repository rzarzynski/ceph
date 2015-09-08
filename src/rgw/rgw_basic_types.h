#ifndef CEPH_RGW_BASIC_TYPES_H
#define CEPH_RGW_BASIC_TYPES_H

#include <string>

#include "include/types.h"

struct rgw_user {
  std::string default_tenant;
  std::string id;

  rgw_user() {}
  rgw_user(const std::string& s) {
    from_str(s);
  }

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(default_tenant, bl);
    ::encode(id, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& bl) {
    DECODE_START(1, bl);
    ::decode(default_tenant, bl);
    ::decode(id, bl);
    DECODE_FINISH(bl);
  }

  void to_str(std::string& str) const {
    if (!default_tenant.empty()) {
      str = default_tenant + ':' + id;
    } else {
      str = id;
    }
  }

  void clear() {
    default_tenant.clear();
    id.clear();
  }

  bool empty() {
    return id.empty();
  }

  string to_str() const {
    string s;
    to_str(s);
    return s;
  }

  void from_str(const std::string& str) {
    ssize_t pos = str.find(':');
    if (pos >= 0) {
      default_tenant = str.substr(0, pos);
      id = str.substr(pos + 1);
    } else {
      default_tenant.clear();
      id = str;
    }
  }

  rgw_user& operator=(const string& str) {
    from_str(str);
    return *this;
  }

  int compare(const rgw_user& u) const {
    int r = default_tenant.compare(u.default_tenant);
    if (r != 0)
      return r;

    return id.compare(u.id);
  }
  int compare(const string& str) const {
    rgw_user u(str);
    return compare(u);
  }

  bool operator!=(const rgw_user& rhs) const {
    return (compare(rhs) != 0);
  }
  bool operator==(const rgw_user& rhs) const {
    return (compare(rhs) == 0);
  }
  bool operator<(const rgw_user& rhs) const {
    if (default_tenant < rhs.default_tenant) {
      return true;
    } else if (default_tenant > rhs.default_tenant) {
      return false;
    }
    return (id < rhs.id);
  }
};
WRITE_CLASS_ENCODER(rgw_user)


class JSONObj;

void decode_json_obj(rgw_user& val, JSONObj *obj);
void encode_json(const char *name, const rgw_user& val, Formatter *f);

inline ostream& operator<<(ostream& out, const rgw_user &u) {
  string s;
  u.to_str(s);
  return out << s;
}


#endif
