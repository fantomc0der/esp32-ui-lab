// preferences_stub.cpp — the process-local store behind Preferences.h.

#include "Preferences.h"

#include <cstring>
#include <map>
#include <string>

namespace {

// Namespace -> key -> value. Static so it outlives any Preferences instance,
// the way NVS outlives the handle that opened it.
std::map<std::string, std::map<std::string, std::string>> &store() {
  static std::map<std::string, std::map<std::string, std::string>> s;
  return s;
}

}  // namespace

bool Preferences::begin(const char *name, bool read_only) {
  if (!name) return false;
  ns_ = name;
  read_only_ = read_only;
  open_ = true;
  // A read-only open of a namespace that does not exist yet fails on real NVS,
  // and bindings_sys.cpp depends on that: it is how "nothing pinned" is
  // distinguished from "pinned to something" on a fresh device.
  if (read_only && store().find(name) == store().end()) {
    open_ = false;
    return false;
  }
  return true;
}

void Preferences::end() {
  open_ = false;
  ns_ = nullptr;
}

size_t Preferences::putString(const char *key, const char *value) {
  if (!open_ || read_only_ || !key || !value) return 0;
  store()[ns_][key] = value;
  return strlen(value);
}

size_t Preferences::getString(const char *key, char *buf, size_t len) {
  if (!open_ || !key || !buf || len == 0) return 0;
  auto ns_it = store().find(ns_);
  if (ns_it == store().end()) return 0;
  auto it = ns_it->second.find(key);
  if (it == ns_it->second.end()) return 0;
  const std::string &v = it->second;
  // Refuse rather than truncate when the value does not fit, which is what the
  // real implementation does. Truncating would be the more forgiving choice and
  // the wrong one: a caller that mis-sizes its buffer would then get a shortened
  // path that looks valid, and the host would hide a bug the device would hit.
  if (v.size() + 1 > len) return 0;
  memcpy(buf, v.data(), v.size());
  buf[v.size()] = '\0';
  return v.size();
}

bool Preferences::remove(const char *key) {
  if (!open_ || read_only_ || !key) return false;
  auto ns_it = store().find(ns_);
  if (ns_it == store().end()) return false;
  return ns_it->second.erase(key) != 0;
}

void Preferences::host_clear() { store().clear(); }
