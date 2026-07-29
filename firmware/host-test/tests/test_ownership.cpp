// test_ownership.cpp — the JSValue lifetime rules, which is where this repo's
// real bugs have been.
//
// Both use-after-frees in the project's history are reproduced here as
// regression cases, and both are documented in docs/runtime-architecture.md:
//
//   1. A timer callback that stops its own timer. `const t = lv.timer(ms, () =>
//      t.stop())` is the one-shot idiom, and it frees the binding holding the
//      last reference to the closure that is mid-call. JS_Call only borrows
//      func_obj, so without the trampoline's own dup this frees the running
//      closure underneath the interpreter (a LoadProhibited panic on hardware).
//
//   2. A widget handle used after its container was cleaned. Writing through a
//      stale handle used to silently corrupt the heap; jsvm_arg_widget() now
//      validates every handle and throws a JS TypeError instead.
//
// app/selftest.js covers both on hardware. The difference here is ASan: on the
// board a leaked dup is invisible and a freed-closure call sometimes survives by
// luck, whereas here either one fails the build.

#include "host_test.h"

using namespace host_test;

namespace {

// ---- regression 1: a timer that stops itself -------------------------------

void timer_stopping_itself_is_safe() {
  const char *kSrc = R"JS(
    globalThis.fired = 0;
    const t = lv.timer(20, () => { globalThis.fired++; t.stop(); });
  )JS";

  check("ownership: self-stopping timer script evaluates", run_script(kSrc));

  // Well past the interval: a one-shot must fire exactly once however long we
  // wait, and must not be re-entered after freeing its own binding.
  host_tick(500);

  // Assert through the script itself rather than reaching into the VM: this is
  // the same observation app/selftest.js makes, so both targets check one thing.
  const size_t mark = host_serial_mark();
  jsvm_repl_line("console.log('fired=' + globalThis.fired)");
  check("ownership: self-stopping timer fires exactly once",
        host_serial_contains_since(mark, "fired=1"));

  jsvm_stop();
  host_settle();
}

// A timer that stops itself while other timers are still live. The list-unlink
// path is what this exercises: the binding freed sits in the middle of g_timers
// rather than at the head, and getting the doubly-linked unlink wrong there
// corrupts the list without necessarily crashing straight away.
void stopping_one_timer_leaves_others() {
  const char *kSrc = R"JS(
    globalThis.a = 0; globalThis.b = 0; globalThis.c = 0;
    const t1 = lv.timer(20, () => { globalThis.a++; });
    const t2 = lv.timer(20, () => { globalThis.b++; t2.stop(); });
    const t3 = lv.timer(20, () => { globalThis.c++; });
  )JS";

  check("ownership: three-timer script evaluates", run_script(kSrc));
  host_tick(200);

  const size_t mark = host_serial_mark();
  jsvm_repl_line("console.log('b=' + globalThis.b)");
  check("ownership: the self-stopping one of three fires once",
        host_serial_contains_since(mark, "b=1"));

  // The survivors must have kept ticking after their neighbour was unlinked.
  const size_t mark2 = host_serial_mark();
  jsvm_repl_line("console.log('survived=' + (globalThis.a > 1 && globalThis.c > 1))");
  check("ownership: the other two keep firing",
        host_serial_contains_since(mark2, "survived=true"));

  jsvm_stop();
  host_settle();
}

// ---- regression 2: a stale widget handle ----------------------------------

void stale_handle_throws_rather_than_corrupting() {
  // The handle outlives the widget: clean() deletes the child, then the script
  // writes through the handle it kept. Must be a JS TypeError, not a write into
  // freed memory. Under ASan a regression here is a use-after-free report.
  const char *kSrc = R"JS(
    const scr = lv.screen();
    const box = lv.obj(scr, { w: 100, h: 50 });
    const label = lv.label(box, { text: "doomed" });
    scr.clean();
    let threw = false;
    try { label.set({ text: "after free" }); } catch (e) { threw = true; }
    console.log('stale=' + threw);
  )JS";

  const size_t mark = host_serial_mark();
  check("ownership: stale-handle script evaluates", run_script(kSrc));
  check("ownership: writing through a stale handle throws",
        host_serial_contains_since(mark, "stale=true"));

  jsvm_stop();
  host_settle();
}

// Reading through a stale handle has to fail the same way. Easy to get wrong by
// validating on the write path only, which would leave a getter dereferencing a
// freed object.
void stale_handle_read_also_throws() {
  const char *kSrc = R"JS(
    const scr = lv.screen();
    const box = lv.obj(scr, { w: 100, h: 50 });
    const slider = lv.slider(box, { value: 30 });
    scr.clean();
    let threw = false;
    try { slider.value(); } catch (e) { threw = true; }
    console.log('staleread=' + threw);
  )JS";

  const size_t mark = host_serial_mark();
  check("ownership: stale-read script evaluates", run_script(kSrc));
  check("ownership: reading through a stale handle throws",
        host_serial_contains_since(mark, "staleread=true"));

  jsvm_stop();
  host_settle();
}

// ---- the two release paths for event bindings ------------------------------

// A handler on the screen object and a handler on a child are released by
// different code: the child's by the LV_EVENT_DELETE hook during
// lv_obj_clean(), the screen's only by jsvm_stop()'s explicit sweep, because
// lv_obj_clean() does not delete the screen it is cleaning. Both must run, and
// neither may double-free.
void screen_and_child_handlers_both_release() {
  const size_t base = host_heap_live_bytes();

  const char *kSrc = R"JS(
    const scr = lv.screen();
    scr.on("click", () => {});
    scr.on("press", () => {});
    const b = lv.button(scr, { text: "tap" });
    b.on("click", () => {});
    b.on("longpress", () => {});
  )JS";

  check("ownership: mixed-handler script evaluates", run_script(kSrc));
  host_tick(50);
  jsvm_stop();
  host_settle();

  check_eq("ownership: screen and child handlers both released",
           host_heap_live_bytes(), base);
}

// Many handlers on one widget, then deleting just that widget: every binding
// must come back through the single DELETE hook.
void deleting_a_widget_releases_all_its_handlers() {
  const size_t base = host_heap_live_bytes();

  const char *kSrc = R"JS(
    const scr = lv.screen();
    const b = lv.button(scr, { text: "many" });
    for (let i = 0; i < 12; i++) b.on("click", () => {});
    b.delete();
  )JS";

  check("ownership: many-handler script evaluates", run_script(kSrc));
  host_settle();
  jsvm_stop();
  host_settle();

  check_eq("ownership: deleting a widget releases all its handlers",
           host_heap_live_bytes(), base);
}

// Timers with no .stop() anywhere, so teardown is the only release point. This
// is the case the g_timers sweep in jsvm_stop() exists for.
void timers_left_running_at_teardown_are_released() {
  const size_t base = host_heap_live_bytes();

  const char *kSrc = R"JS(
    for (let i = 0; i < 6; i++) lv.timer(20, () => {});
  )JS";

  check("ownership: unstopped-timer script evaluates", run_script(kSrc));
  host_tick(120);
  jsvm_stop();
  host_settle();

  check_eq("ownership: timers left running at teardown are released",
           host_heap_live_bytes(), base);
}

// Calling .stop() twice must be a no-op the second time. timer_release() sets
// the wrapper's opaque to null precisely so a later .stop() finds nothing;
// without that this is a double free.
void stopping_a_timer_twice_is_a_no_op() {
  const char *kSrc = R"JS(
    const t = lv.timer(20, () => {});
    t.stop();
    let threw = false;
    try { t.stop(); } catch (e) { threw = true; }
    console.log('doublestop_threw=' + threw);
  )JS";

  const size_t mark = host_serial_mark();
  check("ownership: double-stop script evaluates", run_script(kSrc));
  check("ownership: stopping a timer twice does not throw",
        host_serial_contains_since(mark, "doublestop_threw=false"));

  host_tick(100);
  jsvm_stop();
  host_settle();
}

}  // namespace

int main() {
  host_lvgl_begin();

  timer_stopping_itself_is_safe();
  stopping_one_timer_leaves_others();
  stale_handle_throws_rather_than_corrupting();
  stale_handle_read_also_throws();
  screen_and_child_handlers_both_release();
  deleting_a_widget_releases_all_its_handlers();
  timers_left_running_at_teardown_are_released();
  stopping_a_timer_twice_is_a_no_op();

  const int rc = report("test_ownership");
  host_lvgl_end();
  return rc;
}
