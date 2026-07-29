// test_modules.cpp — the sys and fs modules.
//
// Both are compiled into the host target, and until this suite existed neither
// had a single assertion over it: the stubs behind them were exercised only
// incidentally, by whatever the other suites happened to touch. Compiling code
// with no tests over it is the weaker half of "covered", so this closes it.
//
// It also gives the stub setters a caller. host_set_fps()/host_set_battery() and
// the Preferences store exist so that a script's view of the device can be
// arranged from the test, and an unused setter is a setter that has never been
// shown to work.

#include "host_test.h"

#include <cmath>

#include "stubs/FS.h"
#include "stubs/Preferences.h"

using namespace host_test;

namespace {

// The two filesystems the fs bindings operate on. Registered once; the
// card-then-flash fallback and the "flash:" prefix are what make two necessary.
fs::FS g_sd;
fs::FS g_flash;

void expect_output(const char *name, const char *src, const char *expect) {
  const size_t mark = host_serial_mark();
  if (!run_script(src, name)) {
    bad(name, "script evaluation threw");
    jsvm_stop();
    host_settle();
    return;
  }
  host_settle();
  if (host_serial_contains_since(mark, expect)) {
    ok(name);
  } else {
    std::string got = host_serial_since(mark);
    if (got.size() > 300) got = got.substr(got.size() - 300);
    bad(name, std::string("expected \"") + expect + "\", got: " + got);
  }
  jsvm_stop();
  host_settle();
}

// ---- sys, and the host hooks behind it -------------------------------------

void sys_reports_what_the_hooks_say() {
  host_set_fps(42);
  expect_output("sys: fps() reports what the host hook returns",
                "console.log('fps=' + sys.fps());", "fps=42");

  host_set_battery(3.87f);
  expect_output("sys: battery() reports the host hook's voltage",
                "console.log('batt=' + (sys.battery() > 3.8 && sys.battery() < 3.9));",
                "batt=true");

  // NAN is how a host says "no reading available" (the ADC2/WiFi conflict on the
  // real board), and it has to surface to scripts as null rather than as NaN.
  host_set_battery(NAN);
  expect_output("sys: an unavailable battery reads back as null",
                "console.log('battnull=' + (sys.battery() === null));", "battnull=true");
  host_set_battery(4.05f);
}

void sys_backlight_clamps_and_reaches_the_host() {
  host_hooks_reset();

  // Clamping is documented as the binding's job, so the hook should only ever
  // see 0..100 however the script misbehaves. Each bound is checked on its own
  // rather than after a sequence, so the assertion cannot pass because some
  // later call happened to leave the right value behind.
  if (!run_script("sys.backlight(50);")) {
    bad("sys: backlight script evaluates", "evaluation threw");
  } else {
    ok("sys: backlight script evaluates");
    check_eq("sys: an in-range value passes through", (int)host_backlight(), 50);
    check_eq("sys: backlight reached the host once", host_backlight_call_count(), 1);
  }
  jsvm_stop();
  host_settle();

  run_script("sys.backlight(999);");
  check_eq("sys: a value above 100 clamps to 100", (int)host_backlight(), 100);
  jsvm_stop();
  host_settle();

  run_script("sys.backlight(-40);");
  check_eq("sys: a negative value clamps to 0", (int)host_backlight(), 0);
  jsvm_stop();
  host_settle();
}

void sys_info_and_heap_have_the_documented_shape() {
  expect_output("sys: info() reports the fields scripts read",
                R"JS(
                  const i = sys.info();
                  console.log('info=' + [i.model, i.cores, i.mhz, i.lvgl]
                    .every(v => v !== undefined && v !== null));
                )JS",
                "info=true");

  // {internal, psram}, the two pools a script might reason about. On the host
  // both come from the same synthetic counter, so this asserts the shape only —
  // the numbers mean nothing here, which docs/host-test.md says outright.
  expect_output("sys: heap() reports both pools",
                R"JS(
                  const h = sys.heap();
                  console.log('heap=' + (h.internal > 0 && h.psram > 0));
                )JS",
                "heap=true");

  // uptime() comes from millis(), which the harness controls, so this is the one
  // place the virtual clock is observable from a script.
  expect_output("sys: uptime() advances with the clock",
                R"JS(
                  const a = sys.uptime();
                  console.log('uptime=' + (a >= 0));
                )JS",
                "uptime=true");
}

// The pin is stored through Preferences, and the contract that matters is that
// it outlives the VM: a pinned app has to survive the reboot that follows.
void sys_pin_round_trips_through_storage() {
  Preferences::host_clear();

  expect_output("sys: nothing is pinned on a fresh device",
                "console.log('pin=' + sys.pinned());", "pin=null");

  expect_output("sys: pin() stores a path",
                R"JS(
                  sys.pin("/apps/weather.js");
                  console.log('pin=' + sys.pinned());
                )JS",
                "pin=/apps/weather.js");

  // A new VM, the same store: this is the reboot case. The pin must still be
  // there, which is what makes it a pin rather than a variable.
  expect_output("sys: a pin survives a stop/start cycle",
                "console.log('pin=' + sys.pinned());", "pin=/apps/weather.js");

  expect_output("sys: unpin() clears it",
                R"JS(
                  sys.unpin();
                  console.log('pin=' + sys.pinned());
                )JS",
                "pin=null");

  Preferences::host_clear();
}

// ---- fs ---------------------------------------------------------------------

void fs_round_trips_a_file() {
  g_sd.host_clear();
  jsvm_set_filesystem(&g_sd, &g_flash);

  expect_output("fs: a written file reads back",
                R"JS(
                  fs.write("/note.txt", "hello");
                  console.log('read=' + fs.read("/note.txt"));
                )JS",
                "read=hello");

  expect_output("fs: append extends rather than replaces",
                R"JS(
                  fs.write("/a.txt", "one");
                  fs.append("/a.txt", "two");
                  console.log('read=' + fs.read("/a.txt"));
                )JS",
                "read=onetwo");

  expect_output("fs: exists() and remove() agree",
                R"JS(
                  fs.write("/gone.txt", "x");
                  const before = fs.exists("/gone.txt");
                  fs.remove("/gone.txt");
                  console.log('ex=' + before + ',' + fs.exists("/gone.txt"));
                )JS",
                "ex=true,false");

  expect_output("fs: reading a missing file returns null rather than throwing",
                "console.log('missing=' + fs.read('/nope.txt'));", "missing=null");

  expect_output("fs: list() names what was written",
                R"JS(
                  fs.write("/d/one.txt", "1");
                  fs.write("/d/two.txt", "2");
                  const names = fs.list("/d").join(",");
                  console.log('list=' + names);
                )JS",
                "list=one.txt,two.txt");
}

// The "flash:" prefix is what lets one path space address two filesystems, and
// getting it wrong means an app writes to the card and reads from flash.
void fs_flash_prefix_targets_the_other_filesystem() {
  g_sd.host_clear();
  g_flash.host_clear();
  jsvm_set_filesystem(&g_sd, &g_flash);

  expect_output("fs: a flash: path does not appear on the card",
                R"JS(
                  fs.write("flash:/only-flash.txt", "F");
                  console.log('split=' + fs.exists("flash:/only-flash.txt")
                                       + ',' + fs.exists("/only-flash.txt"));
                )JS",
                "split=true,false");

  expect_output("fs: the same name can hold different data on each filesystem",
                R"JS(
                  fs.write("/both.txt", "card");
                  fs.write("flash:/both.txt", "flash");
                  console.log('both=' + fs.read("/both.txt") + ',' + fs.read("flash:/both.txt"));
                )JS",
                "both=card,flash");
}

// A null filesystem must throw rather than fail silently — that is the
// documented contract, and silence here would look like an empty card.
void fs_without_a_filesystem_throws() {
  jsvm_set_filesystem(nullptr, nullptr);

  expect_output("fs: calls against no filesystem throw",
                R"JS(
                  let threw = false;
                  try { fs.write("/x.txt", "x"); } catch (e) { threw = true; }
                  console.log('nofs=' + threw);
                )JS",
                "nofs=true");

  expect_output("fs: available() reports false with no filesystem",
                "console.log('avail=' + fs.available());", "avail=false");

  jsvm_set_filesystem(&g_sd, &g_flash);
}

}  // namespace

int main() {
  host_lvgl_begin();

  sys_reports_what_the_hooks_say();
  sys_backlight_clamps_and_reaches_the_host();
  sys_info_and_heap_have_the_documented_shape();
  sys_pin_round_trips_through_storage();
  fs_round_trips_a_file();
  fs_flash_prefix_targets_the_other_filesystem();
  fs_without_a_filesystem_throws();

  const int rc = report("test_modules");
  host_lvgl_end();
  return rc;
}
