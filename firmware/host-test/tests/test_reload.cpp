// test_reload.cpp — starting and stopping the same script must return to zero.
//
// This is the test issue #16 names as the one worth having first, and the reason
// is procedural rather than theoretical: reload correctness is currently checked
// by hand, five cycles at a time, by watching a serial log. That catches a leak
// that grows fast and nothing else.
//
// Here the same loop runs 25 times with two independent leak checks running, and
// it is worth being exact about which one sees what, because they do not overlap
// the way "belt and braces" would suggest:
//
//   host_heap_live_bytes()  counts allocations made through heap_caps_* — so the
//                           JS heap, and every JSValue inside a binding. It sees
//                           a duped callback that was never released, and it sees
//                           it at the end of the cycle rather than at exit.
//
//   LeakSanitizer           sees anything still allocated and unreachable when the
//                           process ends, whatever allocator made it. EventBinding
//                           and TimerBinding come from plain malloc (jsvm_core.cpp
//                           197 and 253), so this is the only check that sees those
//                           structs leak.
//
// The gap worth naming: a binding left linked on g_events at teardown is both
// malloc-side (invisible to the counters) and still reachable from a global (so
// LSan does not call it a leak). Neither check catches that directly. What does
// catch it is the JSValues it still holds, which the counters do see — which is
// why the assertion below is on bytes and blocks rather than on the list itself.

#include "host_test.h"

using namespace host_test;

namespace {

// Deliberately exercises everything with a release point: widgets under the
// screen (released by the LV_EVENT_DELETE hook), a handler bound to the screen
// object itself (which lv_obj_clean does not delete, so only jsvm_stop's sweep
// reaches it), a running timer, and a stopped one.
const char *kScript = R"JS(
  const scr = lv.screen().set({ bg: "#101418" });
  scr.on("click", () => { globalThis.screenClicks = (globalThis.screenClicks|0) + 1; });

  const col = lv.obj(scr, { w: "100%", h: "100%", flex: "column" });
  for (let i = 0; i < 8; i++) {
    const row = lv.obj(col, { w: "100%", h: 24 });
    lv.label(row, { text: "row " + i, font: 14 });
    lv.button(row, { text: "go" }).on("click", () => { globalThis.hits = (globalThis.hits|0) + 1; });
  }

  const slider = lv.slider(col, { w: "90%", value: 40 });
  slider.on("change", () => {});

  const live = lv.timer(50, () => { globalThis.ticks = (globalThis.ticks|0) + 1; });
  const once = lv.timer(50, () => {});
  once.stop();
)JS";

void reload_cycles_do_not_grow() {
  // One warm-up cycle first. The very first start allocates things that are
  // legitimately kept afterwards (LVGL's theme, font glyph caches, the display's
  // own bookkeeping), so measuring from zero would report that one-time cost as
  // a leak. The invariant under test is that cycle N+1 costs the same as cycle
  // N, which is what a leak would break.
  if (!run_script(kScript)) {
    bad("reload: warm-up script evaluates", "evaluation threw");
    return;
  }
  host_tick(200);
  jsvm_stop();
  host_settle();

  const size_t base_bytes = host_heap_live_bytes();
  const size_t base_blocks = host_heap_live_blocks();

  const int kCycles = 25;
  for (int i = 0; i < kCycles; i++) {
    if (!run_script(kScript)) {
      bad("reload: script evaluates every cycle", "evaluation threw");
      return;
    }
    // Let the live timer actually fire a few times, so each cycle has run
    // callbacks rather than only registered them. A trampoline that leaked a
    // dup per invocation would otherwise go unnoticed.
    host_tick(200);

    // Confirm the timer really fired, once, on the first cycle. Without this the
    // whole loop would still pass if timers silently never ran — and then it
    // would be asserting that not running anything leaks nothing, which is true
    // and worthless.
    if (i == 0) {
      const size_t mark = host_serial_mark();
      jsvm_repl_line("console.log('ticks=' + ((globalThis.ticks|0) > 1))");
      check("reload: the script's timer fires during a cycle",
            host_serial_contains_since(mark, "ticks=true"));
    }

    jsvm_stop();
    host_settle();
  }

  check_eq("reload: 25 cycles leak no bytes", host_heap_live_bytes(), base_bytes);
  check_eq("reload: 25 cycles leak no blocks", host_heap_live_blocks(), base_blocks);
}

// A script that throws part-way leaves a partial UI up, and the supervisor's
// contract is that jsvm_stop() still cleans up after it. That path is easy to
// get wrong precisely because the script never finished registering things.
void failed_eval_still_tears_down() {
  const size_t base = host_heap_live_bytes();

  const char *kThrows = R"JS(
    const scr = lv.screen();
    lv.label(scr, { text: "built before the throw" });
    lv.button(scr, { text: "also built" }).on("click", () => {});
    lv.timer(50, () => {});
    throw new Error("halfway");
  )JS";

  const bool evaluated = run_script(kThrows, "<throws>");
  check("reload: a throwing script reports failure", !evaluated);

  jsvm_stop();
  host_settle();
  check_eq("reload: teardown after a throw leaks nothing", host_heap_live_bytes(), base);
}

// Starting without stopping is allowed — jsvm_start() tears down any previous
// script itself — and is what sys.launch() ends up doing. Worth its own case:
// it is the one path where teardown runs with a live context about to be
// replaced rather than from a quiet state.
void start_over_start_is_clean() {
  if (!run_script(kScript)) {
    bad("reload: first script evaluates", "evaluation threw");
    return;
  }
  host_tick(100);

  const size_t after_first = host_heap_live_bytes();

  // Start again with no intervening stop, five times.
  for (int i = 0; i < 5; i++) {
    if (!run_script(kScript)) {
      bad("reload: restart without stop evaluates", "evaluation threw");
      return;
    }
    host_tick(100);
  }

  // Each start replaced the last, so the resting cost should be what one script
  // costs, not six. An exact match is too strict (QuickJS's own allocation
  // pattern varies slightly with atom reuse), so allow a small margin while
  // still failing on anything resembling a per-cycle leak of a whole script.
  const size_t now = host_heap_live_bytes();
  const size_t slack = after_first / 4;
  check("reload: restart without stop does not accumulate",
        now <= after_first + slack);

  jsvm_stop();
  host_settle();
}

}  // namespace

int main() {
  host_lvgl_begin();

  reload_cycles_do_not_grow();
  failed_eval_still_tears_down();
  start_over_start_is_clean();

  const int rc = report("test_reload");
  host_lvgl_end();
  return rc;
}
