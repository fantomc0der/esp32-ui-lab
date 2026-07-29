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
// "b=1" is a prefix of "b=10", so a test asserting a timer fired once passed while
// it was firing ten times. Matching the trailing newline closes that: no value can
// satisfy an assertion meant for a longer one.
//
// Only the right edge is anchored, so "b=1\n" would also be satisfied by a line
// ending "ab=1". Nothing in the suites collides that way, and anchoring the left
// edge with a leading newline is not safe here — jsvm_report_exception() prints a
// stack trace with no trailing newline, so a line is not guaranteed to start after
// one. Keep keys distinct rather than relying on the match to separate them.
//
// Use this for a value read back through the REPL; expect_output() does the same
// matching for one a script printed as it ran.
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

// Runs a script that prints one `key=value` line and checks that exact line
// appeared. The script is the assertion; this plumbs the output.
//
// Matching is whole-line for the same reason check_printed is: a substring search
// makes "sel=1" satisfy an assertion about "sel=10". No assertion in the suites
// currently has a value space wide enough to collide, but relying on that is
// relying on arithmetic rather than on the check, and the next value added would
// arm the trap.
inline void expect_output(const char *name, const char *src, const char *expect) {
  const size_t mark = host_serial_mark();
  if (!run_script(src, name)) {
    bad(name, "script evaluation threw");
    jsvm_stop();
    host_settle();
    return;
  }
  host_settle();
  if (host_serial_contains_since(mark, (std::string(expect) + "\n").c_str())) {
    ok(name);
  } else {
    std::string got = host_serial_since(mark);
    // Trim to keep a failure readable: the VM's own boot lines precede it.
    if (got.size() > 300) got = got.substr(got.size() - 300);
    bad(name, std::string("expected \"") + expect + "\", got: " + got);
  }
  jsvm_stop();
  host_settle();
}

// The count line, and the process's exit status. Non-zero on any failure is what
// makes ctest (and CI) notice.
inline int report(const char *suite) {
  printf("\n%s: %d passed, %d failed\n", suite, g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace host_test
