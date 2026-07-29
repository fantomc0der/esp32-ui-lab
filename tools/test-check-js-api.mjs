// test-check-js-api.mjs — the widget-kind check's tests.
//
//   node tools/test-check-js-api.mjs
//
// Two halves, and the second is the one that earns the feature. One: the
// constraint table really is read out of the C, so it cannot drift from the
// firmware. Two: the scan finds a wrong-kind call and, much more importantly,
// stays quiet everywhere its evidence is thin — a `.push(` in a string, in a
// comment, in a regex, on an array, on a function parameter, on a variable that
// was reassigned. A checker that cries wolf is worse than no checker, because
// the next red result gets ignored.

import { readFileSync, readdirSync, mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { execFileSync } from "node:child_process";
import { deriveModel, findWrongKindCalls } from "./widget-methods.mjs";

let passed = 0;
const failures = [];

function eq(actual, expected, what) {
  const a = JSON.stringify(actual), b = JSON.stringify(expected);
  if (a === b) { passed++; return; }
  failures.push([what, b, a]);
}

function ok(cond, what) {
  if (cond) { passed++; return; }
  failures.push([what, "true", "false"]);
}

// ---------------------------------------------------------------- the C table

const SRC = "firmware/lvgl-js-bindings/src";
const cSource = readdirSync(SRC)
  .filter(f => f.endsWith(".cpp"))
  .map(f => readFileSync(join(SRC, f), "utf8"))
  .join("\n");

const model = deriveModel(cSource);

// Every maker resolves to the LVGL class it builds. If this drops a tag, the
// tool has stopped seeing part of the widget vocabulary.
eq(model.classOfTag.get("label"), "label", "lv.label builds an lv_label");
eq(model.classOfTag.get("button"), "button", "lv.button builds an lv_button");
eq(model.classOfTag.get("switch"), "switch", "lv.switch builds an lv_switch");
eq(model.classOfTag.size, 17, "all 17 makers are mapped");

// The restrictions, as the methods themselves state them.
const only = m => [...(model.classesOfMethod.get(m) ?? [])].sort();
eq(only("push"), ["chart"], "push() is chart-only");
eq(only("add"), ["list"], "add() is list-only");
eq(only("addTab"), ["tabview"], "addTab() is tabview-only");
eq(only("target"), ["keyboard"], "target() is keyboard-only");

// The methods that work on anything must not pick up a restriction, or every
// script using them starts failing.
for (const m of ["set", "on", "value", "clean", "delete", "index", "bounds"]) {
  ok(!model.classesOfMethod.has(m), `${m}() is not restricted to a kind`);
}

// The failure mode that matters most: a table this stops recognising must stop
// the tool, not empty out. An empty table accepts every script, so the check
// would go quiet at exactly the moment the C stopped matching its assumptions.
function throws(cSrc, what) {
  try {
    deriveModel(cSrc);
    failures.push([what, "an error", "no error"]);
  } catch {
    passed++;
  }
}
throws("", "nothing to read at all");
throws(cSource.replace("kMakers[]", "kWidgets[]"), "the maker table renamed");
throws(cSource.replaceAll("wproto", "widgetProto"), "the prototype renamed");
throws(cSource.replaceAll("lv_obj_check_type", "lv_obj_is_kind"), "the guards renamed");

// ---------------------------------------------------------------- the scan

const scan = src => findWrongKindCalls(src, model);

// Reports exactly the methods named, in order.
function flags(src, expected, what) {
  eq(scan(src).map(p => `${p.receiver}.${p.method}`), expected, what);
}
const quiet = (src, what) => flags(src, [], what);

// ---- what it must catch

flags(`lv.label(p, {}).push(3)`, ["lv.label().push"], "the issue's own example");
flags(`const l = lv.label(s, {});\nl.push(3);`, ["l.push"], "through a local");
flags(`const l = lv.label(s, {});\nl.add("x");`, ["l.add"], "add() on a non-list");
flags(`const o = lv.obj(s, {});\no.addTab("t");`, ["o.addTab"], "addTab() on a non-tabview");
flags(`const b = lv.button(s, {});\nb.target(t);`, ["b.target"], "target() on a non-keyboard");

// .set() and .on() hand the widget back, so the kind survives them.
flags(`lv.label(p, {}).set({w: 4}).push(3)`, ["lv.label().push"], "through a chained set()");
flags(`const l = lv.label(s, {});\nl.set({}).on("click", f).push(1);`,
      ["l.push"], "through a chain on a local");

flags(`const l = lv.label(s, {});\nl?.push(3);`, ["l.push"], "through optional chaining");

// A substitution is code, and the transform that produced it is not going to
// have made it any safer.
flags("const l = lv.label(s, {});\nconst t = `${l.push(3)}`;", ["l.push"],
      "inside a template substitution");

// Two wrong calls are two reports, at their own lines.
eq(scan(`const l = lv.label(s, {});\nl.push(1);\nl.add("x");`).map(p => p.line),
   [2, 3], "each report carries its line");

// ---- what it must not catch

quiet(`lv.chart(p, {}).push(3)`, "push() on a chart is fine");
quiet(`const c = lv.chart(s, {});\nc.set({}).push(3);`, "a chart through a chain");
quiet(`const t = lv.tabview(s, {});\nt.addTab("a");`, "addTab() on a tabview");
quiet(`const l = lv.list(s, {});\nl.add("row");`, "add() on a list");
quiet(`const k = lv.keyboard(s, {});\nk.target(ta);`, "target() on a keyboard");

quiet(`const rows = [];\nrows.push(1);`, "an array is not a widget");
quiet(`const l = lv.label(s, {});\nl.value("x");\nl.set({}).index(2);\nl.bounds();`,
      "the unrestricted methods work on any kind");

// A widget that came from somewhere this scan cannot follow.
quiet(`const row = list.add("x");\nrow.push(1);`, "a widget from .add()");
quiet(`const l = lv.label(s, {});\nl.parent.push(1);`, "a property is not the local");
quiet(`chart.current.push(1);`, "a ref's .current");
quiet(`const t = lv.timer(10, f);\nt.push(1);`, "lv.timer() is not a maker");
quiet(`const s = lv.screen();\ns.push(1);`, "lv.screen() has no declared kind");

// Rebinding. Each of these makes the name mean something the scan cannot pin
// down, so it has to stop trusting the name entirely.
quiet(`let w = lv.label(s, {});\nw = lv.chart(s, {});\nw.push(1);`, "reassigned");
quiet(`function f(l) { l.push(1); }\nconst l = lv.label(s, {});`, "a function parameter");
quiet(`const go = l => l.push(1);\nconst l = lv.label(s, {});`, "an arrow parameter");
quiet(`const m = { run(l) { l.push(1); } };\nconst l = lv.label(s, {});`, "a method parameter");
quiet(`const l = lv.label(s, {});\nfor (const l of xs) l.push(1);`, "rebound by for-of");
quiet(`if (a) { const l = lv.label(s, {}); } else { const l = lv.chart(s, {}); }\nl.push(1);`,
      "two declarations that disagree");
quiet(`const { l } = obj;\nl.push(1);`, "a destructured name");

// A name is still trusted when the file only ever says one thing about it,
// even in two places — otherwise the check would be worth very little in a
// file that builds several screens.
flags(`{ const l = lv.label(s, {}); l.set({}); }\n{ const l = lv.label(s, {}); l.push(1); }`,
      ["l.push"], "two declarations that agree");

// Things that only look like code.
quiet(`const l = lv.label(s, {});\nconsole.log("l.push(3)");`, "inside a string");
quiet(`const l = lv.label(s, {});\n// l.push(3)`, "inside a line comment");
quiet(`const l = lv.label(s, {});\n/* l.push(3) */`, "inside a block comment");
quiet(`const l = lv.label(s, {});\nconst re = /l\\.push\\(/;`, "inside a regex literal");
quiet("const l = lv.label(s, {});\nconst t = `l.push(3)`;", "inside a template literal");

// A regex holding an unbalanced quote used to be the thing that derailed a
// scanner like this one.
quiet(`const l = lv.label(s, {});\nconst re = /it's/;\nconst c = lv.chart(s, {});\nc.push(1);`,
      "a regex holding an apostrophe");

// The opt-out, which app/selftest.js needs: calling the wrong method is the
// test there.
quiet(`const o = lv.obj(s, {});\no.push(1);  // check-js-api: wrong kind on purpose`,
      "a line marked on purpose");
flags(`const o = lv.obj(s, {});\no.push(1);\no.add("x");  // check-js-api: wrong kind on purpose`,
      ["o.push"], "the marker covers only its own line");

// ---------------------------------------------------------------- end to end

// The acceptance criterion as written: a bad script fails the actual tool.
const dir = mkdtempSync(join(tmpdir(), "check-js-api-"));
try {
  writeFileSync(join(dir, "bad.js"), `const l = lv.label(lv.screen(), {});\nl.push(3);\n`);
  let code = 0, output = "";
  try {
    // Piped, not inherited: the failure it prints is the expected result here,
    // and letting it through would read as this test's own output.
    execFileSync(process.execPath, ["tools/check-js-api.mjs", dir],
                 { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] });
  } catch (e) {
    code = e.status;
    output = (e.stdout ?? "") + (e.stderr ?? "");
  }
  eq(code, 1, "the tool exits 1 on a wrong-kind call");
  ok(/\.push\(\) only works on lv\.chart/.test(output), "and says what would have accepted it");
} finally {
  rmSync(dir, { recursive: true, force: true });
}

// ---------------------------------------------------------------- report

if (failures.length) {
  for (const [what, expected, actual] of failures) {
    console.log(`FAIL ${what}\n  expected: ${expected}\n  actual:   ${actual}`);
  }
  console.log(`\n${passed} passed, ${failures.length} failed`);
  process.exit(1);
}
console.log(`ok — ${passed} checks passed`);
