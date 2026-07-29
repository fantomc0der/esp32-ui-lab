// Preferences.h — the host stand-in for NVS-backed preferences.
//
// Used by bindings_sys.cpp for the app pin (sys.pin()), which needs exactly
// begin/end/getString/putString/remove. Backed by a process-local store, so it
// behaves like NVS within a run without persisting beyond it: the store outlives
// any single Preferences instance, the way NVS outlives the handle that opened it.
//
// What it deliberately does NOT give you is "the pin survives a restart" across a
// jsvm_stop()/jsvm_start() pair. bindings_sys.cpp caches the pin in a file-static
// that nothing resets, so after any write every later read is answered from that
// cache rather than from here. Observing a real read means seeding the store with
// host_seed() before anything has populated the cache — see test_modules.cpp and
// docs/host-test.md, which record how that was verified.
#pragma once

#include <stddef.h>

class Preferences {
 public:
  bool begin(const char *name, bool read_only = false);
  void end();

  size_t putString(const char *key, const char *value);

  // Copies into buf and returns the number of bytes written *including* the
  // terminator, which is what the ESP32 core's implementation returns. Returns 0
  // and leaves buf untouched when the key is absent or the value does not fit —
  // the caller relies on that to keep its default, and on the refuse-rather-than-
  // truncate behaviour so a mis-sized buffer cannot yield a plausible-looking
  // truncated path.
  size_t getString(const char *key, char *buf, size_t len);

  bool remove(const char *key);

  // Host-side reset, so one test's pin cannot leak into the next.
  static void host_clear();

  // Writes straight into the store, without going through a begin()/end() pair.
  // This is how a test arranges "the device already had this in NVS before it
  // booted" — which is the only way to observe that a read really goes to
  // storage, because bindings_sys.cpp caches the pin in a file-static on first
  // read and nothing in the library resets it.
  static void host_seed(const char *ns, const char *key, const char *value);

 private:
  const char *ns_ = nullptr;
  bool read_only_ = false;
  bool open_ = false;
};
