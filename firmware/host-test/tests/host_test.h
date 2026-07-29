// host_test.h — the smallest test vocabulary that does the job.
//
// No framework: a dependency to install in CI, for check/CHECK_EQ and a counter.
// The output shape deliberately matches app/selftest.js ("PASS name" / "FAIL
// name — why", ending in a count), so a host run and a hardware run read the
// same way and neither needs a separate mental model.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "host_harness.h"
#include "js_bindings.h"
#include "stubs/Arduino.h"
#include "stubs/esp_heap_caps.h"
#include "stubs/host_serial.h"

namespace host_test {

inline int g_pass = 0;
inline int g_fail = 0;

inline void ok(const char *name) {
  g_pass++;
  printf("PASS %s\n", name);
}

inline void bad(const char *name, const std::string &why) {
  g_fail++;
  printf("FAIL %s — %s\n", name, why.c_str());
}

inline void check(const char *name, bool cond) {
  if (cond) ok(name); else bad(name, "expected true");
}

template <typename A, typename B>
inline void check_eq(const char *name, A got, B want) {
  if (got == static_cast<A>(want)) {
    ok(name);
  } else {
    char buf[160];
    snprintf(buf, sizeof(buf), "got %lld, want %lld", (long long)got, (long long)want);
    bad(name, buf);
  }
}

// Asserts that a script printed exactly `key=value` and not merely something
// starting that way.
//
// This exists because a plain substring search is a trap for numeric results:
// "b=1" is a prefix of "b=10", so a test asserting a timer fired once passed
// while it was firing ten times. Every count assertion goes through here, which
// prints a newline-delimited line and matches the delimiter too, so no value can
// satisfy an assertion meant for a different one.
inline void check_printed(const char *name, size_t mark, const char *key,
                          const char *value) {
  const std::string want = std::string(key) + "=" + value + "\n";
  const std::string got = host_serial_since(mark);
  if (got.find(want) != std::string::npos) {
    ok(name);
    return;
  }
  // Show what the key actually held, which is the useful half of a failure.
  const std::string prefix = std::string(key) + "=";
  const size_t at = got.find(prefix);
  std::string actual = "(never printed)";
  if (at != std::string::npos) {
    const size_t end = got.find('\n', at);
    actual = got.substr(at, end == std::string::npos ? std::string::npos : end - at);
  }
  bad(name, std::string("wanted ") + key + "=" + value + ", got " + actual);
}

// Evaluating a script is the unit most of these tests work in, so the two
// helpers below wrap the start/settle and stop/verify halves.

// Starts a script, runs the queue once, and reports whether evaluation
// succeeded. Mirrors what the supervisor does with a freshly loaded file.
inline bool run_script(const char *src, const char *name = "<test>") {
  const bool ok_eval = jsvm_start(src, name);
  host_settle();
  return ok_eval;
}

// The count line, and the process's exit status. Non-zero on any failure is what
// makes ctest (and CI) notice.
inline int report(const char *suite) {
  printf("\n%s: %d passed, %d failed\n", suite, g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace host_test
