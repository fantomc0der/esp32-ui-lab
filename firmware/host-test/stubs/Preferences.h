// Preferences.h — the host stand-in for NVS-backed preferences.
//
// Used by bindings_sys.cpp for the app pin (sys.pin()), which needs exactly
// begin/end/getString/putString/remove. Backed by a process-local store, so it
// behaves like NVS within a run without persisting beyond it: the store outlives
// any single Preferences instance, which is what makes "the pin survives a
// restart" observable across a jsvm_stop()/jsvm_start() pair.
#pragma once

#include <stddef.h>

class Preferences {
 public:
  bool begin(const char *name, bool read_only = false);
  void end();

  size_t putString(const char *key, const char *value);

  // Copies into buf and returns the length written, excluding the terminator.
  // Leaves buf untouched and returns 0 when the key is absent, which is what
  // the caller relies on to keep its default.
  size_t getString(const char *key, char *buf, size_t len);

  bool remove(const char *key);

  // Host-side reset, so one test's pin cannot leak into the next.
  static void host_clear();

 private:
  const char *ns_ = nullptr;
  bool read_only_ = false;
  bool open_ = false;
};
