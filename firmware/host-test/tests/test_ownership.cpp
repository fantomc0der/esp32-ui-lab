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
//      The layer has two trampolines and the same hazard on both, so the event
//      one is here too: a click handler that cleans the screen it is attached to.
//
//   2. A widget handle used after its container was cleaned. Writing through a
//      stale handle used to silently corrupt the heap; jsvm_arg_widget() now
//      validates every handle and throws a JS TypeError instead.
//
// app/selftest.js covers both on hardware. The difference here is ASan: on the
// board a leaked dup is invisible and a freed-closure call sometimes survives by
// luck, whereas here either one fails the build.

#include <lvgl.h>

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
  // check_printed rather than a substring search, because "fired=1" is a prefix
  // of "fired=10" — which is exactly what a broken self-stop produces over this
  // many ticks.
  const size_t mark = host_serial_mark();
  jsvm_repl_line("console.log('fired=' + globalThis.fired)");
  check_printed("ownership: self-stopping timer fires exactly once", mark, "fired", "1");

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
  check_printed("ownership: the self-stopping one of three fires once", mark, "b", "1");

  // The survivors must have kept ticking after their neighbour was unlinked, and
  // must have ticked the number of times the elapsed 200 ms allows. Asserting the
  // count rather than "more than one" means a .stop() that leaked into its
  // neighbours — unlinking the wrong node — shows up here rather than only under
  // ASan.
  const size_t mark2 = host_serial_mark();
  jsvm_repl_line("console.log('a=' + globalThis.a)");
  check_printed("ownership: an untouched neighbour fires every interval", mark2, "a", "10");

  const size_t mark3 = host_serial_mark();
  jsvm_repl_line("console.log('c=' + globalThis.c)");
  check_printed("ownership: the other neighbour does too", mark3, "c", "10");

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
  check_printed("ownership: writing through a stale handle throws", mark, "stale", "true");

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
  check_printed("ownership: reading through a stale handle throws", mark, "staleread",
                "true");

  jsvm_stop();
  host_settle();
}

// ---- regression 1's twin, on the event trampoline --------------------------

// The trampoline dups its fn and widget for the duration of the call, for the
// same reason the timer one does: a handler is allowed to destroy the widget it
// is attached to, which fires LV_EVENT_DELETE and frees the binding holding the
// last reference mid-call.
//
// Every name in kEvents is a code an input device raises, and the harness
// registers none, so nothing a script can do from inside the VM reaches
// event_trampoline. lv_obj_send_event() from here does, and it is not a
// contrivance: it is the same entry point LVGL's own widgets use to raise an
// event outside a touch, and lv_indev_active() being null is exactly what a
// handler sees then. What this cannot reach is the other branch, fn(widget,x,y),
// which needs a real indev to have a point to report.
void event_trampoline_runs_the_handler() {
  const char *kSrc = R"JS(
    globalThis.clicks = 0;
    globalThis.gotSelf = false;
    const b = lv.button(lv.screen(), { text: "tap" });
    b.on("click", (self) => { globalThis.clicks++; globalThis.gotSelf = (self === b); });
  )JS";

  check("ownership: click-handler script evaluates", run_script(kSrc));

  lv_obj_t *btn = lv_obj_get_child(lv_screen_active(), 0);
  check("ownership: the button is on the screen", btn != nullptr);
  lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
  host_settle();

  // Exactly once, not merely non-zero, and both directions are reachable. Zero is
  // what a trampoline that is never entered gives: that was this suite's state
  // before this case existed. Above one is what a callback registered on both of
  // event_send_core's passes would give, since it calls lv_event_send() twice and
  // only the filter keeps a normal handler out of the first. check_printed matches
  // the whole line, so clicks=1 cannot be satisfied by clicks=10.
  const size_t mark = host_serial_mark();
  jsvm_repl_line("console.log('clicks=' + globalThis.clicks)");
  check_printed("ownership: a dispatched event calls the handler exactly once", mark,
                "clicks", "1");

  // The argument is the widget wrapper, not a fresh one: jsvm_bind_event dups the
  // `this` of .on(), so identity holds. Without this the first assertion would
  // pass on a trampoline that called fn with no arguments at all.
  const size_t mark2 = host_serial_mark();
  jsvm_repl_line("console.log('gotself=' + globalThis.gotSelf)");
  check_printed("ownership: the handler receives its own widget", mark2, "gotself", "true");

  jsvm_stop();
  host_settle();
}

// The hazard the fn dup exists for. The handler cleans the screen it sits on, so
// the button is deleted while LVGL is dispatching to it: event_delete_cb runs
// event_unlink_and_free() on the binding whose fn is the closure currently
// executing. That drops the closure's last reference, and QuickJS frees its
// bytecode while the interpreter is still reading it — verified by deleting the
// dup, which reports a heap-use-after-free in JS_CallInternal with
// free_function_bytecode on the freeing stack. On hardware it was a
// LoadProhibited panic. The widget dup is a separate question and not covered
// here; see the case below.
//
// LVGL supports the shape, so a failure here is ours rather than a misuse of the
// API. What makes it legal is a mark-and-check pair rather than anything about the
// event list: lv_obj_send_event() pushes the lv_event_t onto a global in-flight
// list, lv_obj_delete() calls lv_event_mark_deleted() to set e->deleted on any
// whose target is going away, and lv_event_send() re-tests e->deleted after every
// callback returns and breaks out before touching the freed list again
// (lv_event.c:125, :141, :318). The back_array_head copy at :117 is a separate
// concern, freeing the array's contents once the header is gone.
void a_handler_may_clean_its_own_screen() {
  // Two counters, one either side of the clean(). `entered` alone would only say
  // the handler started, since the increment happens before the delete — what
  // proves it ran to the end is `finished`, set from a statement the interpreter
  // reaches only after the closure's binding has been freed underneath it. That
  // also catches a failure that unwinds the handler without aborting the process,
  // e.g. an exception thrown out of the delete path, which jsvm_call_reporting()
  // swallows into jsvm_report_exception() rather than propagating.
  const char *kSrc = R"JS(
    globalThis.entered = 0;
    globalThis.finished = 0;
    const b = lv.button(lv.screen(), { text: "self-destruct" });
    b.on("click", () => { globalThis.entered++; lv.screen().clean(); globalThis.finished++; });
  )JS";

  check("ownership: self-cleaning-handler script evaluates", run_script(kSrc));

  lv_obj_t *btn = lv_obj_get_child(lv_screen_active(), 0);
  check("ownership: the self-cleaning button is on the screen", btn != nullptr);
  lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
  host_settle();

  const size_t mark = host_serial_mark();
  jsvm_repl_line("console.log('entered=' + globalThis.entered)");
  check_printed("ownership: a handler that cleans its own screen is entered once", mark,
                "entered", "1");

  const size_t mark2 = host_serial_mark();
  jsvm_repl_line("console.log('finished=' + globalThis.finished)");
  check_printed("ownership: and runs past the delete to its last statement", mark2,
                "finished", "1");

  // And the widget really is gone, so the assertion above is about surviving the
  // delete rather than about a clean() that quietly did nothing.
  check_eq("ownership: the handler's clean() emptied the screen",
           lv_obj_get_child_count(lv_screen_active()), 0u);

  jsvm_stop();
  host_settle();
}

// Regression 2's rule, on the event path: a handler that outlives its own widget
// must get a TypeError from it, not a dangling lv_obj_t. The two cases above delete
// the widget too, but neither touches it afterwards, so nothing there exercises
// jsvm_arg_widget() from inside a live dispatch.
//
// What this deliberately does *not* prove is the trampoline's widget dup, and the
// reason is not the one it looks like. Deleting that dup together with its matching
// JS_FreeValue leaves every assertion in this suite green, and not because the
// wrapper is freed and the throw coincides: in *this* case the wrapper is never
// freed. JS_Call always passes JS_CALL_FLAG_COPY_ARGV (quickjs.c:20378), so the
// condition at :17632 holds whatever argc is, and a bytecode callee copies its
// arguments into its own frame at :17655, one js_dup per declared parameter. The
// handler here declares `self`, so it gets a reference of its own for the frame's
// lifetime, and the binding dropping its reference mid-handler takes the refcount
// to 1 rather than to 0. Measured rather than argued: with the dup gone, a handler
// that deletes its widget and then churns 20000 allocations still reads `self` with
// no sanitizer report.
//
// Be exact about the scope, because two neighbouring shapes do not get that
// protection. The copy is sized by the callee's declared parameter count
// (arg_allocated_size = b->arg_count at :17633, gated at :17651), so a handler
// declaring no parameters gets no copy and arg_buf stays the borrowed argv from
// :17645 — which is the shape of a_handler_may_clean_its_own_screen two cases up,
// where what keeps the wrapper alive is that script's own `const b`. And the copy
// is bytecode-only: :17620-17628 hands argv straight to call_func for a native or
// bound-to-native callable, which .on() accepts. Neither shape reads the widget
// after triggering the delete today, which is why the mutation still survives, but
// the dup is what makes that a property of the trampoline rather than of the
// handler someone happens to write. It stays for that reason, and because deleting
// it while leaving the JS_FreeValue *is* caught here, as an over-release. The fn
// dup is covered outright, by the case above this one. See docs/host-test.md.
//
// One trap worth recording, since it cost a round: `typeof self` looks like it
// touches the wrapper and does not. A JSValue carries its tag inline, so typeof
// reads the stack copy and never dereferences the object. Calling a method does.
void a_handler_whose_widget_is_unreferenced_by_script() {
  const char *kSrc = R"JS(
    globalThis.tag = "";
    lv.button(lv.screen(), { text: "unheld" }).on("click", (self) => {
      lv.screen().clean();
      let threw = false;
      try { self.bounds(); } catch (e) { threw = true; }
      globalThis.tag = threw ? "threw" : "returned";
    });
  )JS";

  check("ownership: unheld-widget script evaluates", run_script(kSrc));

  lv_obj_t *btn = lv_obj_get_child(lv_screen_active(), 0);
  check("ownership: the unheld button is on the screen", btn != nullptr);
  lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
  host_settle();

  const size_t mark = host_serial_mark();
  jsvm_repl_line("console.log('tag=' + globalThis.tag)");
  check_printed("ownership: a handler touching its deleted widget gets a TypeError",
                mark, "tag", "threw");

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
  check_printed("ownership: stopping a timer twice does not throw", mark,
                "doublestop_threw", "false");

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
  event_trampoline_runs_the_handler();
  a_handler_may_clean_its_own_screen();
  a_handler_whose_widget_is_unreferenced_by_script();
  screen_and_child_handlers_both_release();
  deleting_a_widget_releases_all_its_handlers();
  timers_left_running_at_teardown_are_released();
  stopping_a_timer_twice_is_a_no_op();

  const int rc = report("test_ownership");
  host_lvgl_end();
  return rc;
}
