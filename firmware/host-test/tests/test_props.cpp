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
  // A hex string with and without the leading '#', and a plain number, are all
  // accepted forms. Be clear about how little this proves: the binding exposes no
  // style getter, so nothing here can read a colour back, and these cases assert
  // only that the three forms parse without throwing and leave a live widget.
  // Measured, not assumed — making parse_color() return false unconditionally
  // leaves both cases below passing. The colours themselves are verified by
  // looking at the panel, and docs/host-test.md says so.
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
  // Position, not absence of a throw. apply_props walks kAligns and does nothing
  // on no match (bindings_lv.cpp:239-241) — the case above proves that — so a
  // try/catch here could never fire, and a name deleted or misspelled in the C
  // table would leave a "does it throw" assertion reporting PASS. Verified: two
  // names mangled in kAligns, suite still green.
  //
  // Instead each name is applied to an identical child and its bounds recorded.
  // Requiring all nine to be pairwise distinct catches a dropped or misspelled
  // entry, because an unrecognised name leaves the child where an unaligned one
  // sits and so collides with whichever real name lands there. The box is much
  // larger than the child so the nine anchors cannot coincide for want of room.
  //
  // Distinctness alone would not catch a *permuted* table: swapping two codes
  // leaves nine distinct positions and the count still reads 9. So the geometry
  // is asserted alongside it, which pins the mapping rather than the cardinality
  // — every top-* above every bottom-*, every *-left left of every *-right, and
  // center strictly inside both.
  expect_output("props: every alignment name puts the widget somewhere different",
                R"JS(
                  const box = lv.obj(lv.screen(), { w: 300, h: 150, pad: 0, border: 0 });
                  const names = ["center","top-left","top-mid","top-right",
                                 "bottom-left","bottom-mid","bottom-right",
                                 "left-mid","right-mid"];
                  const at = {}, seen = new Set();
                  for (const a of names) {
                    const r = lv.obj(box, { w: 20, h: 20, align: a }).bounds();
                    at[a] = r;
                    seen.add(r.x + ',' + r.y);
                  }
                  const hi = (ns, k) => Math.max(...ns.map(n => at[n][k]));
                  const lo = (ns, k) => Math.min(...ns.map(n => at[n][k]));
                  const tops = ["top-left","top-mid","top-right"];
                  const bots = ["bottom-left","bottom-mid","bottom-right"];
                  const lefts = ["top-left","bottom-left","left-mid"];
                  const rights = ["top-right","bottom-right","right-mid"];
                  const ordered = hi(tops,'y') < lo(bots,'y')
                               && hi(lefts,'x') < lo(rights,'x')
                               && at['center'].y > hi(tops,'y') && at['center'].y < lo(bots,'y')
                               && at['center'].x > hi(lefts,'x') && at['center'].x < lo(rights,'x');
                  console.log('aligns=' + seen.size + '/' + ordered);
                )JS",
                "aligns=9/true");

  // One name in that set is not actually covered, and it cannot be from here.
  // LV_ALIGN_TOP_LEFT puts a child exactly where an unaligned child already sits
  // (measured at 15,15 above, and 35,35 with pad: 20 — padding shifts both
  // identically), so "top-left" missing from kAligns is indistinguishable by
  // position from "top-left" working. The count above still reads nine. Two other
  // approaches were tried and do not work either: comparing against a deliberately
  // unrecognised name matches for the same reason, and adding padding shifts both
  // by the same amount.
  //
  // So: the eight non-origin names are covered by position, and top-left is
  // covered only in that it does not throw. Distinguishing it would need the
  // binding to report the alignment back, which it does not. This is recorded in
  // docs/host-test.md rather than papered over with a case that looks like it
  // checks something it does not.
  expect_output("props: top-left is accepted (position cannot distinguish it)",
                R"JS(
                  const box = lv.obj(lv.screen(), { w: 300, h: 150, pad: 0, border: 0 });
                  const b = lv.obj(box, { w: 20, h: 20, align: "top-left" });
                  console.log('topleft=' + (b.index() >= 0));
                )JS",
                "topleft=true");

  // Flex flow is observable the same way: row lays children out along x and
  // column along y, so the first two children's coordinates say which flow the
  // table actually applied. An unrecognised name means no flow is set, and the
  // children stack at the origin like the column case — hence asserting row
  // separately from column rather than only that neither threw.
  expect_output("props: row flows along x and column along y",
                R"JS(
                  const scr = lv.screen();
                  const r = lv.obj(scr, { w: 200, h: 60, pad: 0, border: 0, flex: "row" });
                  const r1 = lv.obj(r, { w: 20, h: 20 }), r2 = lv.obj(r, { w: 20, h: 20 });
                  const c = lv.obj(scr, { w: 60, h: 200, pad: 0, border: 0, flex: "column" });
                  const c1 = lv.obj(c, { w: 20, h: 20 }), c2 = lv.obj(c, { w: 20, h: 20 });
                  const rb1 = r1.bounds(), rb2 = r2.bounds();
                  const cb1 = c1.bounds(), cb2 = c2.bounds();
                  const rowOk = rb2.x > rb1.x && rb2.y === rb1.y;
                  const colOk = cb2.y > cb1.y && cb2.x === cb1.x;
                  console.log('flows=' + (rowOk && colOk));
                )JS",
                "flows=true");

  // row-wrap and column-wrap differ from their unwrapped forms only once the
  // children overflow, so each gets its own case: enough children to force a
  // second line, then assert where they actually went. Without both, one of the
  // two wrap entries in kFlexFlows would be unasserted — a misspelling falls back
  // to no flow at all, which apply_props does not report.
  //
  // "Something wrapped" is not enough to tell the two apart, which is the
  // permutation gap the review named: six 30px children in a 100px box produce a
  // 3x2 grid either way, so both flows put *some* child below the first and some
  // child right of it. Measured, the discriminator is the second child —
  // row-wrap gives 15:15 53:15 15:53 …, column-wrap gives 15:15 15:53 53:15 …,
  // so a row fills across before wrapping down and a column the reverse. Each
  // case asserts the grid formed *and* which way the fill ran, so swapping the
  // two codes fails both.
  expect_output("props: row-wrap fills across, then wraps onto a second line",
                R"JS(
                  const box = lv.obj(lv.screen(), { w: 100, h: 100, pad: 0, border: 0,
                                                    flex: "row-wrap" });
                  const kids = [];
                  for (let i = 0; i < 6; i++) kids.push(lv.obj(box, { w: 30, h: 30 }));
                  const b = kids.map(k => k.bounds());
                  const grid = b.some(r => r.x === b[0].x && r.y > b[0].y);
                  const across = b[1].y === b[0].y && b[1].x > b[0].x;
                  console.log('wrapped=' + (grid && across));
                )JS",
                "wrapped=true");

  // The mirror, for the fourth entry. Same grid, filled the other way: the second
  // child goes below the first and the wrap starts a new column to the right. An
  // unrecognised name leaves every child at the origin, which fails both halves.
  expect_output("props: column-wrap fills downward, then wraps into a second column",
                R"JS(
                  const box = lv.obj(lv.screen(), { w: 100, h: 100, pad: 0, border: 0,
                                                    flex: "column-wrap" });
                  const kids = [];
                  for (let i = 0; i < 6; i++) kids.push(lv.obj(box, { w: 30, h: 30 }));
                  const b = kids.map(k => k.bounds());
                  const grid = b.some(r => r.y === b[0].y && r.x > b[0].x);
                  const downward = b[1].x === b[0].x && b[1].y > b[0].y;
                  console.log('colwrapped=' + (grid && downward));
                )JS",
                "colwrapped=true");

  // kFlexAligns, the sixth and last string→enum table in apply_props, and the
  // one the review noted as unasserted. The box is deliberately *wider* than its
  // children need, and that free space is the mechanism: a main-axis placement
  // decides how to distribute what is left over, so END starts at `free`, CENTER
  // at free/2, BETWEEN spreads the gap to free/(n-1), AROUND to free/n/2 and
  // EVENLY to free/(n+1). Measured, the three x positions per name are:
  // start 15,53,91 / end 109,147,185 / center 62,100,138 / between 15,100,185 /
  // around 30,99,168 / evenly 38,99,160. Six distinct triples.
  //
  // "start" is the value apply_props initialises place[] to before consulting the
  // table, so it is the one name a misspelling is indistinguishable from — the
  // same limit top-left has, and stated for the same reason.
  //
  // As with kAligns, distinctness alone would not see a permuted table, so the
  // ordering is pinned too: the three one-sided placements advance strictly left
  // to right, and `between` is the only name that puts the first child where
  // `start` does *and* the last where `end` does.
  expect_output("props: every flexAlign name distributes children differently",
                R"JS(
                  const names = ["start","end","center","between","around","evenly"];
                  const at = {}, seen = new Set();
                  for (const a of names) {
                    const box = lv.obj(lv.screen(), { w: 200, h: 40, pad: 0, border: 0,
                                                      flex: "row", flexAlign: a });
                    const k = [lv.obj(box, { w: 30, h: 20 }), lv.obj(box, { w: 30, h: 20 }),
                               lv.obj(box, { w: 30, h: 20 })];
                    at[a] = k.map(x => x.bounds().x);
                    seen.add(at[a].join(','));
                  }
                  const x0 = n => at[n][0], xl = n => at[n][2];
                  const pinned = x0("start") < x0("center") && x0("center") < x0("end")
                              && x0("between") === x0("start") && xl("between") === xl("end")
                              && x0("around") < x0("evenly");
                  console.log('flexaligns=' + seen.size + '/' + pinned);
                )JS",
                "flexaligns=6/true");

  // flexAlign takes two shapes, and the case above only ever passes a string, so
  // only the JS_IsString(v) half of the fetch at bindings_lv.cpp:343-344 runs.
  // The array form is the one every shipped app uses (app/apps/tasks.js) and the
  // one docs/binding-api.md documents, and it is the half reaching
  // JS_GetPropertyUint32 — a JSValue refcount pair inside a two-shape prop is
  // exactly what this suite is for.
  //
  // apply_props passes place[1] to both the cross and the track argument, and
  // those are different things in LVGL: cross_place positions an item inside its
  // own track, track_place positions the tracks inside the container. A track's
  // cross size is its widest child, so a single-child track leaves cross_place
  // nothing to move, and an earlier version of this case measured track_place
  // while claiming both — stubbing the track argument to START failed it and
  // stubbing the cross argument did not. Two children of different widths
  // separate them: the narrow child's right edge against the wide child's is
  // inside the track wherever the track itself ended up, so this observes
  // cross_place and not track_place. Measured, the stubs now fail the other way
  // round. track_place is consequently unasserted, which is the honest state:
  // asserting it would need a second track, and a column whose tracks wrap.
  expect_output("props: flexAlign accepts the [main, cross] array form",
                R"JS(
                  const mk = (cross) => {
                    const box = lv.obj(lv.screen(), { w: 200, h: 120, pad: 0, border: 0,
                                                      flex: "column",
                                                      flexAlign: ["start", cross] });
                    const wide = lv.obj(box, { w: 120, h: 20 });
                    const narrow = lv.obj(box, { w: 30, h: 20 });
                    const w = wide.bounds(), n = narrow.bounds();
                    return { flush: n.x + n.w === w.x + w.w, left: n.x === w.x };
                  };
                  console.log('cross=' + (mk("end").flush && mk("start").left));
                )JS",
                "cross=true");
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
                  console.log('rightkind=' + (tab.index() >= 0));
                )JS",
                "rightkind=true");
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
