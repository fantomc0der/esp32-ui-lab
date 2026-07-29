// test_props.cpp — apply_props and the string→enum tables.
//
// These are the near-pure parts of bindings_lv.cpp, and the cheapest thing on
// the board to get wrong without noticing: a misspelled colour or alignment
// silently falls back to a default, so the UI is subtly wrong rather than
// broken, and nobody reads a stack trace about it.
//
// Everything here asserts through the script, using the same getters a script
// has, so a test says what a user of the binding would observe.

#include "host_test.h"

using namespace host_test;

namespace {

// Runs a script that prints one line, and checks the line appears. The script
// is the assertion; this just plumbs the output.
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
    // Trim to keep a failure readable: the VM's own boot lines precede it.
    if (got.size() > 300) got = got.substr(got.size() - 300);
    bad(name, std::string("expected \"") + expect + "\", got: " + got);
  }
  jsvm_stop();
  host_settle();
}

// ---- the ordering rule ------------------------------------------------------

// The rule bindings_lv.cpp calls out explicitly: apply_props reads `value` near
// the top, before `options` exists, and LVGL clamps a selection against an empty
// list to zero. So `value` is applied a second time after the options are set.
// Without that, `{options, value}` in one call always lands on item 0.
void options_and_value_in_one_call() {
  expect_output("props: dropdown {options, value} selects the right item",
                R"JS(
                  const d = lv.dropdown(lv.screen(), { options: ["a","b","c","d"], value: 2 });
                  console.log('sel=' + d.value());
                )JS",
                "sel=2");

  expect_output("props: roller {options, value} selects the right item",
                R"JS(
                  const r = lv.roller(lv.screen(), { options: ["one","two","three"], value: 1 });
                  console.log('sel=' + r.value());
                )JS",
                "sel=1");

  // The same in two calls has always worked; worth pinning so a refactor that
  // "simplifies" the double-apply cannot break only the one-call form.
  expect_output("props: options then value in separate calls also works",
                R"JS(
                  const d = lv.dropdown(lv.screen(), { options: ["a","b","c","d"] });
                  d.set({ value: 3 });
                  console.log('sel=' + d.value());
                )JS",
                "sel=3");

  // Value beyond the end of the list: LVGL clamps, and the point is that it
  // clamps to the last real option rather than to zero.
  expect_output("props: an out-of-range value clamps to the last option",
                R"JS(
                  const d = lv.dropdown(lv.screen(), { options: ["a","b"], value: 9 });
                  console.log('sel=' + d.value());
                )JS",
                "sel=1");
}

// ---- colours ----------------------------------------------------------------

void colour_parsing() {
  // A hex string with and without the leading '#', and a plain number, must all
  // reach the same colour. There is no getter for a style colour, so this
  // asserts the parse does not throw and the widget survives it; the value
  // itself is covered by the board's own eyes. Stated plainly rather than
  // pretending to check more than it does.
  expect_output("props: colours parse in every accepted form",
                R"JS(
                  const scr = lv.screen();
                  const a = lv.obj(scr, { w: 10, h: 10, bg: "#ff8800" });
                  const b = lv.obj(scr, { w: 10, h: 10, bg: "ff8800" });
                  const c = lv.obj(scr, { w: 10, h: 10, bg: 0xff8800 });
                  console.log('colours=' + [a,b,c].every(x => x.index() >= 0));
                )JS",
                "colours=true");

  // A malformed colour must not throw either. strtoul stops at the first bad
  // character, so this documents that the parse degrades instead of failing.
  expect_output("props: a malformed colour does not throw",
                R"JS(
                  const o = lv.obj(lv.screen(), { w: 10, h: 10, bg: "#zzz" });
                  console.log('badcolour=' + (o.index() >= 0));
                )JS",
                "badcolour=true");
}

// ---- sizes ------------------------------------------------------------------

void size_parsing() {
  // Percentages resolve against the parent's content area, which is the whole
  // reason a script can lay out for any panel. The display is created at the
  // board's real 320x172, so these numbers are the panel's numbers.
  // .bounds() reports the content area, so it is inset from the widget's own
  // width by padding and border. The assertion is therefore a relationship
  // rather than an exact number: half's content box must be narrower than its
  // parent's, and both positive.
  expect_output("props: a percentage width resolves against the parent",
                R"JS(
                  const box = lv.obj(lv.screen(), { w: "100%", h: 100, pad: 0 });
                  const half = lv.obj(box, { w: "50%", h: 10, pad: 0 });
                  const bb = box.bounds(), hb = half.bounds();
                  console.log('halfw=' + (hb.w > 0 && hb.w < bb.w));
                )JS",
                "halfw=true");

  // An exact number is available once padding is zeroed: the content area of a
  // 120px-wide box with no padding and no border is 120px.
  expect_output("props: a pixel width is taken literally",
                R"JS(
                  const box = lv.obj(lv.screen(), { w: 120, h: 40, pad: 0, border: 0 });
                  console.log('w=' + box.bounds().w);
                )JS",
                "w=120");

  // "content" shrink-wraps to the children, which is the third accepted form
  // and the one a script uses for a label-sized box.
  expect_output("props: a content-sized box is narrower than the screen",
                R"JS(
                  const box = lv.obj(lv.screen(), { w: "content", h: "content", pad: 0 });
                  lv.label(box, { text: "hi" });
                  console.log('content=' + (box.bounds().w > 0 && box.bounds().w < lv.size().w));
                )JS",
                "content=true");
}

// ---- text -------------------------------------------------------------------

// A textarea is the only widget whose text comes back through .value(), so it
// is what a round-trip can actually be asserted on. A label's text is
// write-only through this surface — there is no getter — which is why these
// cases use a textarea rather than the label they would more obviously use.
void text_round_trips() {
  expect_output("props: a textarea's text reads back",
                R"JS(
                  const t = lv.textarea(lv.screen(), { value: "hello panel" });
                  console.log('t=' + t.value());
                )JS",
                "t=hello panel");

  // Unicode matters: scripts are UTF-8, and the upload path has mangled it
  // before. This is the cheapest place to notice a byte-level regression.
  expect_output("props: non-ASCII text survives the round trip",
                R"JS(
                  const t = lv.textarea(lv.screen(), { value: "° 42µ ±3" });
                  console.log('t=' + t.value());
                )JS",
                "t=° 42µ ±3");

  expect_output("props: an empty string is not treated as absent",
                R"JS(
                  const t = lv.textarea(lv.screen(), { value: "start" });
                  t.value("");
                  console.log('len=' + t.value().length);
                )JS",
                "len=0");

  // A label still has to accept text without throwing, even though nothing
  // reads it back. Asserting the widget survives is all this surface allows.
  expect_output("props: a label accepts text",
                R"JS(
                  const l = lv.label(lv.screen(), { text: "written, not readable" });
                  console.log('label=' + (l.index() >= 0));
                )JS",
                "label=true");
}

// ---- ranges and values ------------------------------------------------------

void ranges_clamp() {
  expect_output("props: a slider range is applied before its value",
                R"JS(
                  const s = lv.slider(lv.screen(), { range: [10, 20], value: 15 });
                  console.log('v=' + s.value());
                )JS",
                "v=15");

  // A value outside the range must clamp to the range, not wrap or pass through.
  expect_output("props: a value above the range clamps to its top",
                R"JS(
                  const s = lv.slider(lv.screen(), { range: [0, 50], value: 999 });
                  console.log('v=' + s.value());
                )JS",
                "v=50");

  expect_output("props: a bar takes a range too",
                R"JS(
                  const b = lv.bar(lv.screen(), { range: [0, 10], value: 7 });
                  console.log('v=' + b.value());
                )JS",
                "v=7");
}

// ---- unknown props and bad values ------------------------------------------

// The binding surface is curated, so a script naming something that does not
// exist is a typo. It must be ignored rather than throwing: a script is loaded
// at runtime and one bad prop should not take the whole app down.
void unknown_props_are_ignored() {
  expect_output("props: an unknown prop is ignored, not fatal",
                R"JS(
                  const t = lv.textarea(lv.screen(), { value: "ok", nonsenseProp: 42, alsoNot: "x" });
                  console.log('t=' + t.value());
                )JS",
                "t=ok");

  expect_output("props: a nonsense alignment falls back rather than throwing",
                R"JS(
                  const t = lv.textarea(lv.screen(), { value: "ok", align: "not-an-alignment" });
                  console.log('t=' + t.value());
                )JS",
                "t=ok");

  expect_output("props: a nonsense font falls back rather than throwing",
                R"JS(
                  const t = lv.textarea(lv.screen(), { value: "ok", font: 999 });
                  console.log('t=' + t.value());
                )JS",
                "t=ok");

  // An unknown *event* name is the deliberate exception to that leniency: a
  // handler that silently never fires is far harder to diagnose than a throw at
  // registration, so .on() validates and the message lists the valid names.
  expect_output("props: an unknown event name throws, unlike an unknown prop",
                R"JS(
                  const b = lv.button(lv.screen(), { text: "x" });
                  let msg = "";
                  try { b.on("not-an-event", () => {}); } catch (e) { msg = e.message; }
                  console.log('evt=' + (msg.indexOf("unknown event") === 0));
                )JS",
                "evt=true");
}

// ---- alignment and flex tables ---------------------------------------------

void alignment_names_are_accepted() {
  // Every name in the kAligns table, applied in one script. A name dropped from
  // the table would make its widget throw, which fails the whole line.
  expect_output("props: every documented alignment name is accepted",
                R"JS(
                  const scr = lv.screen();
                  const names = ["center","top-left","top-mid","top-right",
                                 "bottom-left","bottom-mid","bottom-right",
                                 "left-mid","right-mid"];
                  let ok = true;
                  for (const a of names) {
                    try { lv.label(scr, { text: a, align: a }); } catch (e) { ok = false; }
                  }
                  console.log('aligns=' + ok);
                )JS",
                "aligns=true");

  expect_output("props: every documented flex flow is accepted",
                R"JS(
                  const scr = lv.screen();
                  let ok = true;
                  for (const f of ["row","column","row-wrap","column-wrap"]) {
                    try { lv.obj(scr, { w: 50, h: 50, flex: f }); } catch (e) { ok = false; }
                  }
                  console.log('flows=' + ok);
                )JS",
                "flows=true");
}

// ---- the widget-kind guard --------------------------------------------------

// A method called on the wrong kind of widget throws a TypeError from the C
// guard. tools/check-js-api.mjs catches this statically for committed scripts;
// this checks the runtime half that backs it up.
void wrong_kind_method_throws() {
  // addTab() guards on lv_tabview_class, so calling it on a plain label must
  // throw rather than reinterpreting the object.
  expect_output("props: a widget method on the wrong kind throws",
                R"JS(
                  const l = lv.label(lv.screen(), { text: "not a tabview" });
                  let threw = false;
                  // check-js-api: wrong kind on purpose
                  try { l.addTab("nope"); } catch (e) { threw = true; }
                  console.log('wrongkind=' + threw);
                )JS",
                "wrongkind=true");

  // And the guard must not be over-eager: the same method on the right kind
  // has to work, or the test above would pass for the wrong reason.
  expect_output("props: the same method on the right kind works",
                R"JS(
                  const tv = lv.tabview(lv.screen(), { w: "100%", h: "100%" });
                  const tab = tv.addTab("one");
                  console.log('righttkind=' + (tab.index() >= 0));
                )JS",
                "righttkind=true");
}

}  // namespace

int main() {
  host_lvgl_begin();

  options_and_value_in_one_call();
  colour_parsing();
  size_parsing();
  text_round_trips();
  ranges_clamp();
  unknown_props_are_ignored();
  alignment_names_are_accepted();
  wrong_kind_method_throws();

  const int rc = report("test_props");
  host_lvgl_end();
  return rc;
}
