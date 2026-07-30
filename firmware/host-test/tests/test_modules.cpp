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

  // Each clamp case starts from a resting value that is NOT the bound it expects,
  // and counts the calls, so it cannot pass on a leftover value or on a script that
  // threw before reaching sys.backlight(). host_hooks_reset() rests at 100, which is
  // one of the bounds under test, so the upper case is seeded away from it first.
  host_hooks_reset();
  if (!run_script("sys.backlight(10); sys.backlight(999);")) {
    bad("sys: upper-clamp script evaluates", "evaluation threw");
  } else {
    ok("sys: upper-clamp script evaluates");
    check_eq("sys: both calls reached the host", host_backlight_call_count(), 2);
    check_eq("sys: a value above 100 clamps to 100", (int)host_backlight(), 100);
  }
  jsvm_stop();
  host_settle();

  host_hooks_reset();
  if (!run_script("sys.backlight(-40);")) {
    bad("sys: lower-clamp script evaluates", "evaluation threw");
  } else {
    ok("sys: lower-clamp script evaluates");
    check_eq("sys: the call reached the host", host_backlight_call_count(), 1);
    check_eq("sys: a negative value clamps to 0", (int)host_backlight(), 0);
  }
  jsvm_stop();
  host_settle();
}

// REGRESSION: a no-argument call used to be read as "set it to 0", which on the
// real board dims the panel to its PWM floor and looks like a crash rather than
// a mistake in the script. Both halves are asserted: the script sees a
// TypeError, and the hook is never reached, so the panel cannot have moved.
// host_hooks_reset() rests at 100 and the old bug wrote 0, so a pass here cannot
// be the old behaviour landing on the expected value.
void sys_backlight_without_an_argument_throws() {
  host_hooks_reset();

  expect_output("sys: backlight() with no argument throws",
                R"JS(
                  let threw = false;
                  try { sys.backlight(); } catch (e) { threw = e instanceof TypeError; }
                  console.log('blthrew=' + threw);
                )JS",
                "blthrew=true");

  check_eq("sys: a throwing backlight() never reached the host",
           host_backlight_call_count(), 0);
  check_eq("sys: the level is untouched after a throw", (int)host_backlight(), 100);

  // The same failure one level down: a value that cannot convert to a number
  // left pct at 0 and dimmed the panel, dropping the pending exception on the
  // way. The conversion's own failure has to propagate too.
  host_hooks_reset();
  expect_output("sys: an unconvertible percent throws instead of dimming",
                R"JS(
                  let threw = false;
                  try { sys.backlight({ valueOf() { throw new Error("nope"); } }); }
                  catch (e) { threw = true; }
                  console.log('blconv=' + threw);
                )JS",
                "blconv=true");

  check_eq("sys: an unconvertible percent never reached the host",
           host_backlight_call_count(), 0);
  check_eq("sys: the level is untouched after a failed conversion",
           (int)host_backlight(), 100);
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
  // place the virtual clock is observable from a script. It needs two reads with
  // a tick between them: nothing else in this suite advances the clock, so a
  // single read could only ever assert 0 >= 0, which is true however broken
  // uptime() is.
  if (!run_script("globalThis.t0 = sys.uptime();")) {
    bad("sys: uptime script evaluates", "evaluation threw");
  } else {
    ok("sys: uptime script evaluates");
    host_tick(500);
    const size_t mark = host_serial_mark();
    jsvm_repl_line("console.log('elapsed=' + (sys.uptime() - globalThis.t0))");
    // Exactly the time that was stepped, since the clock is virtual and nothing
    // else moves it. An approximate check here would tolerate uptime() returning
    // a constant offset or the wrong unit.
    check_printed("sys: uptime() advances by exactly the time stepped", mark, "elapsed",
                  "500");
  }
  jsvm_stop();
  host_settle();
}

// The pin, and the one thing a host test has to be careful about here.
//
// bindings_sys.cpp caches the pin in a file-static (g_pinned, guarded by
// g_pinned_loaded) that nothing in the library resets, because on a device the
// process only starts once. So after any call to sys.pin(), every later
// sys.pinned() in this process is answered from that cache rather than from
// storage — a stop/start cycle does not clear it, and a test written as
// "pin, restart, read it back" would pass with storage entirely disconnected.
// Verified by gutting Preferences::putString: all four assertions still passed.
//
// The way to observe a real read is to seed the store before anything has
// populated the cache, which is what a device that already had a pin in NVS
// looks like at boot. That path is asserted first, and deliberately before
// anything calls sys.pin().
void sys_pin_reads_and_writes_storage() {
  Preferences::host_clear();

  // Seeded before the first read of any kind, so this can only come from the
  // store. This is the assertion that proves reading works.
  Preferences::host_seed("jsvm-app", "pinned", "/apps/preseeded.js");
  expect_output("sys: a pin already in storage is read at first use",
                "console.log('pin=' + sys.pinned());", "pin=/apps/preseeded.js");

  // And the write half, checked against the store directly rather than through
  // sys.pinned(), which the cache would answer.
  expect_output("sys: pin() reports the path it was given",
                R"JS(
                  sys.pin("/apps/weather.js");
                  console.log('pin=' + sys.pinned());
                )JS",
                "pin=/apps/weather.js");

  char buf[128] = {0};
  Preferences p;
  if (p.begin("jsvm-app", true)) {
    p.getString("pinned", buf, sizeof(buf));
    p.end();
  }
  check("sys: pin() wrote the path through to storage",
        std::string(buf) == "/apps/weather.js");

  expect_output("sys: unpin() clears the pin",
                R"JS(
                  sys.unpin();
                  console.log('pin=' + sys.pinned());
                )JS",
                "pin=null");

  // unpin() must remove the key, not just blank the cache, or the next boot
  // would come up pinned again.
  char after[128] = {0};
  Preferences p2;
  size_t got = 1;
  if (p2.begin("jsvm-app", true)) {
    got = p2.getString("pinned", after, sizeof(after));
    p2.end();
  }
  check_eq("sys: unpin() removed the key from storage", (int)got, 0);

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

  // Sorted before comparing, deliberately. The in-memory stub happens to return
  // entries in sorted order, but a real FAT directory does not promise any
  // particular order, so asserting the stub's would be testing the stub rather
  // than the binding — and would pass here while telling a reader something
  // untrue about the device.
  expect_output("fs: list() names what was written",
                R"JS(
                  fs.write("/d/one.txt", "1");
                  fs.write("/d/two.txt", "2");
                  const names = fs.list("/d").sort().join(",");
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
  sys_backlight_without_an_argument_throws();
  sys_info_and_heap_have_the_documented_shape();
  sys_pin_reads_and_writes_storage();
  fs_round_trips_a_file();
  fs_flash_prefix_targets_the_other_filesystem();
  fs_without_a_filesystem_throws();

  const int rc = report("test_modules");
  host_lvgl_end();
  return rc;
}
