// test-ui.mjs — the reconciler's tests, run on the PC.
//
//   node tools/test-ui.mjs
//
// app/selftest.js remains the real test of the binding layer, because only the
// board can run it. This covers the part that is pure JavaScript: given a tree
// and then a different tree, does app/lib/ui.js make the right `lv` calls? The
// interesting assertions are about what it did *not* do — a re-render that
// creates no widgets, a reorder that moves rather than rebuilds, a handler
// swapped without a second .on().
//
// It loads app/lib/ui.js as text and evaluates it with a fake `lv` in scope,
// which is the same thing the board does with the real one.

import { readFileSync } from "node:fs";
import { makeLv, makeFs } from "./lv-mock.mjs";

let passed = 0, failed = 0;
const failures = [];

function test(name, fn) {
  return Promise.resolve()
    .then(fn)
    .then(() => { passed++; })
    .catch(e => { failed++; failures.push([name, e]); });
}

function eq(actual, expected, what) {
  const a = typeof actual === "string" ? actual : JSON.stringify(actual);
  const b = typeof expected === "string" ? expected : JSON.stringify(expected);
  if (a !== b) throw new Error(`${what || "value"}:\n  expected: ${b}\n  actual:   ${a}`);
}

function ok(cond, what) {
  if (!cond) throw new Error(what || "expected true");
}

// A fresh runtime per test: the module keeps global scheduling state, and one
// test's pending render must not land in the next one.
function fresh() {
  const env = makeLv();
  const src = readFileSync("app/lib/ui.js", "utf8");
  const load = new Function("lv", "console", src + "\n;return UI;");
  return { ...env, ui: load(env.lv, console) };
}

const settle = () => new Promise(r => setImmediate(r));

// ---------------------------------------------------------------- tests

const tests = [];
const it = (name, fn) => tests.push([name, fn]);

it("mounts a tree in declaration order", () => {
  const { ui, dump } = fresh();
  const { h, render } = ui;
  render([
    h("label", { text: "one" }),
    h("obj", null, h("label", { text: "two" })),
  ]);
  eq(dump(), 'label "one"\nobj\n  label "two"\n');
});

it("empties the root before mounting, so it owns the ordering", () => {
  const { ui, lv, dump } = fresh();
  lv.label(lv.screen(), { text: "left over" });
  ui.render(ui.h("label", { text: "mine" }));
  eq(dump(), 'label "mine"\n');
});

it("text children become the text prop", () => {
  const { ui, dump } = fresh();
  const { h, render } = ui;
  render(h("button", null, "Scan"));
  eq(dump(), 'button "Scan"\n');
});

it("a bare string where a widget belongs becomes a label", () => {
  const { ui, dump } = fresh();
  const { h, render } = ui;
  render(h("obj", null, "hello"));
  eq(dump(), 'obj\n  label "hello"\n');
});

it("re-render patches props instead of recreating widgets", async () => {
  const { ui, stats, dump } = fresh();
  const { h, render, useState } = ui;
  let bump;
  function App() {
    const [n, setN] = useState(0);
    bump = () => setN(n + 1);
    return h("obj", { w: 100 }, h("label", { text: "count " + n, color: "#fff" }));
  }
  render(h(App));
  const created = stats.created;
  bump();
  await settle();
  eq(dump(), 'obj\n  label "count 1"\n');
  eq(stats.created, created, "widgets created by the re-render");
  eq(stats.deleted, 0, "widgets deleted by the re-render");
});

it("only changed props reach .set()", async () => {
  const { ui, patches } = fresh();
  const { h, render, useState } = ui;
  let bump;
  function App() {
    const [n, setN] = useState(0);
    bump = () => setN(n + 1);
    // Everything but value is identical every render, including a fresh array
    // literal for range — which is a different object each time and would be
    // rewritten on every render by an identity comparison.
    return h("slider", { w: 100, h: 20, range: [0, 100], value: n });
  }
  render(h(App));
  patches.length = 0;
  bump();
  await settle();
  eq(patches, [{ tag: "slider", keys: ["value"] }], "the props sent to the widget");
});

it("a label rewrites its text and nothing else", async () => {
  const { ui, patches } = fresh();
  const { h, render, useState } = ui;
  let bump;
  function App() {
    const [n, setN] = useState(0);
    bump = () => setN(n + 1);
    return h("label", { color: "#F0F4F8", font: 20, align: "center" }, "count " + n);
  }
  render(h(App));
  patches.length = 0;
  bump();
  await settle();
  eq(patches, [{ tag: "label", keys: ["text"] }], "the props sent to the widget");
});

it("an unchanged state write renders nothing", async () => {
  const { ui, stats } = fresh();
  const { h, render, useState } = ui;
  let write;
  function App() {
    const [n, setN] = useState(7);
    write = () => setN(7);
    return h("label", { text: String(n) });
  }
  render(h(App));
  const before = stats.sets;
  write();
  await settle();
  eq(stats.sets, before, ".set() calls after writing the same value");
});

it("a handler is registered once and swapped by identity", async () => {
  const { ui, stats, screen, fire } = fresh();
  const { h, render, useState } = ui;
  const seen = [];
  let bump;
  function App() {
    const [n, setN] = useState(0);
    bump = () => setN(n + 1);
    // A different closure every render — the case that would leak an .on() per
    // render if the runtime bound the prop directly.
    return h("button", { text: "go", onClick: () => seen.push(n) });
  }
  render(h(App));
  const handlers = stats.handlers;
  bump();
  await settle();
  bump();
  await settle();
  eq(stats.handlers, handlers, ".on() registrations after two re-renders");
  fire(screen.kids[0], "click");
  eq(seen, [2], "the handler that fired");
});

it("dropping a handler prop stops the callback", async () => {
  const { ui, screen, fire } = fresh();
  const { h, render, useState } = ui;
  const seen = [];
  let disable;
  function App() {
    const [on, setOn] = useState(true);
    disable = () => setOn(false);
    return h("button", on ? { text: "go", onClick: () => seen.push(1) } : { text: "go" });
  }
  render(h(App));
  fire(screen.kids[0], "click");
  disable();
  await settle();
  fire(screen.kids[0], "click");
  eq(seen, [1], "clicks that reached the handler");
});

it("the widgets added to the binding layer are reachable as tags", () => {
  const { ui, dump } = fresh();
  const { h, render } = ui;
  render([
    h("bar", { range: [0, 100], value: 40 }),
    h("checkbox", null, "Enable"),
    h("roller", { options: ["one", "two"], value: 1 }),
    h("dropdown", { options: ["a", "b"] }),
    h("spinner", { duration: 800 }),
    h("led", { color: "#4CAF50", value: true }),
  ]);
  eq(dump(), 'bar\ncheckbox "Enable"\nroller\ndropdown\nspinner\nled\n');
});

it("an options array is compared by contents, not identity", async () => {
  const { ui, patches } = fresh();
  const { h, render, useState } = ui;
  let pick;
  function App() {
    const [n, setN] = useState(0);
    pick = () => setN(1);
    // A fresh array literal every render, as any real app would write it.
    return h("roller", { options: ["red", "green", "blue"], value: n });
  }
  render(h(App));
  patches.length = 0;
  pick();
  await settle();
  eq(patches, [{ tag: "roller", keys: ["value"] }], "only the selection was rewritten");
});

it("a change event carries the widget's value", () => {
  const { ui, screen, fire } = fresh();
  const { h, render } = ui;
  let got = null;
  render(h("slider", { value: 42, onChange: e => { got = e.value; } }));
  fire(screen.kids[0], "change");
  eq(got, 42, "the value on the event");
});

it("removing a child deletes exactly that widget", async () => {
  const { ui, stats, dump } = fresh();
  const { h, render, useState } = ui;
  let hide;
  function App() {
    const [show, setShow] = useState(true);
    hide = () => setShow(false);
    return h("obj", null,
      h("label", { text: "a" }),
      show && h("label", { text: "b" }),
      h("label", { text: "c" }));
  }
  render(h(App));
  const deleted = stats.deleted;
  hide();
  await settle();
  eq(dump(), 'obj\n  label "a"\n  label "c"\n');
  eq(stats.deleted - deleted, 1, "widgets deleted");
});

it("a child appearing in the middle lands in the right place", async () => {
  const { ui, dump } = fresh();
  const { h, render, useState } = ui;
  let show;
  function App() {
    const [on, setOn] = useState(false);
    show = () => setOn(true);
    return h("obj", null,
      h("label", { text: "a" }),
      on && h("label", { text: "b" }),
      h("label", { text: "c" }));
  }
  render(h(App));
  eq(dump(), 'obj\n  label "a"\n  label "c"\n');
  show();
  await settle();
  eq(dump(), 'obj\n  label "a"\n  label "b"\n  label "c"\n', "declaration order after the insert");
});

it("keyed children reorder by moving, not rebuilding", async () => {
  const { ui, stats, dump } = fresh();
  const { h, render, useState } = ui;
  let reverse;
  function App() {
    const [order, setOrder] = useState(["a", "b", "c"]);
    reverse = () => setOrder(["c", "b", "a"]);
    return h("obj", null, ...order.map(k => h("label", { key: k, text: k })));
  }
  render(h(App));
  const created = stats.created, deleted = stats.deleted;
  reverse();
  await settle();
  eq(dump(), 'obj\n  label "c"\n  label "b"\n  label "a"\n');
  eq(stats.created, created, "widgets created by the reorder");
  eq(stats.deleted, deleted, "widgets deleted by the reorder");
  ok(stats.moves > 0, "expected at least one .index() move");
});

it("a keyed insert at the front keeps the existing widgets", async () => {
  const { ui, stats, screen, dump } = fresh();
  const { h, render, useState } = ui;
  let unshift;
  function App() {
    const [items, setItems] = useState(["b", "c"]);
    unshift = () => setItems(["a", "b", "c"]);
    return h("obj", null, ...items.map(k => h("label", { key: k, text: k })));
  }
  render(h(App));
  const wasB = screen.kids[0].kids[0].id;
  const created = stats.created;
  unshift();
  await settle();
  eq(dump(), 'obj\n  label "a"\n  label "b"\n  label "c"\n');
  eq(stats.created - created, 1, "widgets created for one new row");
  eq(screen.kids[0].kids[1].id, wasB, "the surviving widget is the same one");
});

it("changing an element's type replaces it in place", async () => {
  const { ui, dump } = fresh();
  const { h, render, useState } = ui;
  let swap;
  function App() {
    const [big, setBig] = useState(false);
    swap = () => setBig(true);
    return h("obj", null,
      h("label", { text: "top" }),
      big ? h("arc", { value: 1 }) : h("slider", { value: 1 }),
      h("label", { text: "bottom" }));
  }
  render(h(App));
  swap();
  await settle();
  eq(dump(), 'obj\n  label "top"\n  arc\n  label "bottom"\n');
});

it("a removed prop reverts to its neutral value", async () => {
  const { ui, screen } = fresh();
  const { h, render, useState } = ui;
  let show;
  function App() {
    const [busy, setBusy] = useState(true);
    show = () => setBusy(false);
    return h("label", busy ? { text: "x", hidden: true } : { text: "x" });
  }
  render(h(App));
  eq(screen.kids[0].props.hidden, true, "hidden while busy");
  show();
  await settle();
  eq(screen.kids[0].props.hidden, false, "hidden after the prop went away");
});

it("list rows are made by the list and deleted individually", async () => {
  const { ui, stats, dump } = fresh();
  const { h, render, useState } = ui;
  let drop;
  function App() {
    const [rows, setRows] = useState(["one", "two", "three"]);
    drop = () => setRows(["one", "three"]);
    return h("list", { w: 100 }, ...rows.map(r => h("row", { key: r, text: r })));
  }
  render(h(App));
  eq(dump(), 'list\n  row "one"\n  row "two"\n  row "three"\n');
  const created = stats.created;
  drop();
  await settle();
  eq(dump(), 'list\n  row "one"\n  row "three"\n');
  eq(stats.created, created, "widgets created by removing a row");
});

it("tab contents update in place; a different tab set rebuilds the tabview", async () => {
  const { ui, screen, dump } = fresh();
  const { h, render, useState } = ui;
  let bump, addTab;
  function App() {
    const [n, setN] = useState(0);
    const [three, setThree] = useState(false);
    bump = () => setN(n + 1);
    addTab = () => setThree(true);
    return h("tabview", { bar: 30 },
      h("tab", { name: "A" }, h("label", { text: "n=" + n })),
      h("tab", { name: "B" }),
      three && h("tab", { name: "C" }));
  }
  render(h(App));
  const tabview = screen.kids[0].id;
  bump();
  await settle();
  eq(dump(), 'tabview\n  tab name="A"\n    label "n=1"\n  tab name="B"\n',
     "the tabview survived a content change");
  eq(screen.kids[0].id, tabview, "the same tabview widget");
  addTab();
  await settle();
  ok(screen.kids[0].id !== tabview, "a new tab means a new tabview");
  eq(dump(), 'tabview\n  tab name="A"\n    label "n=1"\n  tab name="B"\n  tab name="C"\n');
});

it("effects run after mount and clean up on unmount", async () => {
  const { ui } = fresh();
  const { h, render, useEffect } = ui;
  const log = [];
  function Child() {
    useEffect(() => {
      log.push("up");
      return () => log.push("down");
    }, []);
    return h("label", { text: "x" });
  }
  function App({ show }) {
    return h("obj", null, show && h(Child));
  }
  const tree = render(h(App, { show: true }));
  eq(log, ["up"], "after mount");
  tree.update(h(App, { show: false }));
  eq(log, ["up", "down"], "after the child went away");
});

it("an effect re-runs only when its deps change", async () => {
  const { ui } = fresh();
  const { h, render, useState, useEffect } = ui;
  const log = [];
  let setA, setB;
  function App() {
    const [a, sa] = useState(0);
    const [b, sb] = useState(0);
    setA = sa; setB = sb;
    useEffect(() => { log.push("a" + a); }, [a]);
    return h("label", { text: a + "/" + b });
  }
  render(h(App));
  eq(log, ["a0"]);
  setB(1);
  await settle();
  eq(log, ["a0"], "changing b left the a effect alone");
  setA(1);
  await settle();
  eq(log, ["a0", "a1"]);
});

it("useInterval stops its timer when the component goes away", async () => {
  const { ui, timers, tick } = fresh();
  const { h, render, useState, useInterval } = ui;
  let ticks = 0;
  let hide;
  function Ticker() {
    useInterval(() => { ticks++; }, 100);
    return h("label", { text: "t" });
  }
  function App() {
    const [on, setOn] = useState(true);
    hide = () => setOn(false);
    return h("obj", null, on && h(Ticker));
  }
  render(h(App));
  eq(timers.size, 1, "timers after mount");
  tick();
  eq(ticks, 1);
  hide();
  await settle();
  eq(timers.size, 0, "timers after unmount");
  tick();
  eq(ticks, 1, "the stopped timer did not fire");
});

it("useInterval calls the newest callback, not the one it started with", async () => {
  const { ui, tick } = fresh();
  const { h, render, useState, useInterval } = ui;
  const seen = [];
  let bump;
  function App() {
    const [n, setN] = useState(0);
    bump = () => setN(n + 1);
    useInterval(() => seen.push(n), 100);
    return h("label", { text: String(n) });
  }
  render(h(App));
  tick();
  bump();
  await settle();
  tick();
  eq(seen, [0, 1], "what the interval saw across a re-render");
});

it("refs point at the widget and are cleared on unmount", async () => {
  const { ui } = fresh();
  const { h, render, useRef, useState } = ui;
  let ref, hide;
  function App() {
    const [on, setOn] = useState(true);
    ref = useRef(null);
    hide = () => setOn(false);
    return h("obj", null, on && h("chart", { ref, points: 20 }));
  }
  render(h(App));
  ok(ref.current && ref.current.tag === "chart", "ref after mount");
  hide();
  await settle();
  eq(ref.current, null, "ref after unmount");
});

it("a component that throws is reported and does not take the tree with it", async () => {
  const { ui, dump } = fresh();
  const { h, render, useState } = ui;
  const errors = [];
  const realError = console.error;
  console.error = (...a) => errors.push(a.join(" "));
  try {
    let boom;
    function Bad() {
      const [fail, setFail] = useState(false);
      boom = () => setFail(true);
      if (fail) throw new Error("nope");
      return h("label", { text: "fine" });
    }
    render(h("obj", null, h(Bad)));
    boom();
    await settle();
    ok(errors.some(e => e.includes("nope")), "the failure was reported");
    eq(dump(), 'obj\n  label "fine"\n', "the widgets that did render are still there");
  } finally {
    console.error = realError;
  }
});

it("a component that sets state forever is stopped, not left spinning", async () => {
  const { ui } = fresh();
  const { h, render, useState, useEffect } = ui;
  const errors = [];
  const realError = console.error;
  console.error = (...a) => errors.push(a.join(" "));
  try {
    function Runaway() {
      const [n, setN] = useState(0);
      useEffect(() => { setN(n + 1); });
      return h("label", { text: String(n) });
    }
    render(h(Runaway));
    for (let i = 0; i < 100 && !errors.length; i++) await settle();
    ok(errors.some(e => e.includes("runaway")), "the loop was caught");
  } finally {
    console.error = realError;
  }
});

it("fragments and nested components keep declaration order", async () => {
  const { ui, dump } = fresh();
  const { h, Fragment, render, useState } = ui;
  let grow;
  function Pair({ n }) {
    return h(Fragment, null, h("label", { text: "x" + n }), h("label", { text: "y" + n }));
  }
  function App() {
    const [n, setN] = useState(0);
    grow = () => setN(1);
    return h("obj", null, h("label", { text: "head" }), h(Pair, { n }), h("label", { text: "tail" }));
  }
  render(h(App));
  eq(dump(), 'obj\n  label "head"\n  label "x0"\n  label "y0"\n  label "tail"\n');
  grow();
  await settle();
  eq(dump(), 'obj\n  label "head"\n  label "x1"\n  label "y1"\n  label "tail"\n');
});

it("unmount() takes the whole tree down", () => {
  const { ui, dump } = fresh();
  const { h, render } = ui;
  const tree = render(h("obj", null, h("label", { text: "a" })));
  tree.unmount();
  eq(dump(), "");
});

// ---------------------------------------------------------------- end to end

// The three pieces have been tested apart; this runs one of them all the way
// through. app/apps/tasks.js is what the board is actually given, straight off
// disk: the JSX transform's output, the bundled runtime, and the app, in the
// file the card holds. If any of the three drift, this is what notices.

function runBuiltApp(path, files) {
  const env = makeLv();
  const fs = makeFs(files);
  const src = readFileSync(path, "utf8");
  new Function("lv", "fs", "console", src)(env.lv, fs, console);
  return { ...env, fs };
}

it("the built tasks app renders, reorders and persists", async () => {
  const { screen, dump, fire, stats, fs } = runBuiltApp("app/apps/tasks.js", {});

  const list = screen.kids.find(w => w.tag === "list");
  ok(list, "the app rendered a list");
  eq(list.kids.map(r => r.props.text), [
    "·  Write one in JSX",
    "·  Reorder without a rebuild",
    "·  Read docs/ui-runtime.md",
    "✓  Flash the firmware",
    "✓  Push an app",
  ], "undone tasks first");

  // Tapping the first row marks it done, which sinks it past the other two
  // undone ones. Every row moves; none should be rebuilt.
  const created = stats.created, deleted = stats.deleted;
  const firstRow = list.kids[0];
  fire(firstRow, "click");
  await settle();

  // The sort is stable, so a task that has just been ticked joins the end of
  // the done group rather than the front of it.
  eq(list.kids.map(r => r.props.text), [
    "·  Reorder without a rebuild",
    "·  Read docs/ui-runtime.md",
    "✓  Flash the firmware",
    "✓  Push an app",
    "✓  Write one in JSX",
  ], "the toggled task sank");
  eq(stats.created - created, 0, "widgets created by the reorder");
  eq(stats.deleted - deleted, 0, "widgets deleted by the reorder");
  eq(list.kids[4].id, firstRow.id, "the moved row is the same widget");

  const saved = JSON.parse(fs.read("/tasks.json"));
  eq(saved.filter(t => t.done).length, 3, "done tasks written to storage");
});

it("the built tasks app replaces the screen from inside a click handler", async () => {
  const { screen, dump, fire } = runBuiltApp("app/apps/tasks.js", {});

  const clear = screen.kids.find(w => w.tag === "button");
  ok(clear, "the Clear done button is there while something is done");
  fire(clear, "click");
  await settle();

  // The confirm panel is up and the button that was tapped is gone — deleted
  // by the render its own handler triggered, which is only safe because the
  // render was deferred out of the dispatch.
  ok(clear.dead, "the tapped button was deleted");
  const panel = screen.kids.find(w => w.tag === "obj" && w.kids.some(k => k.tag === "obj"));
  ok(panel, "the confirm panel is up");

  const keep = [];
  const walk = w => { for (const k of w.kids) { if (k.tag === "button") keep.push(k); walk(k); } };
  walk(panel);
  eq(keep.map(b => b.kids.find(c => c.synthetic)?.props.text), ["Clear", "Keep"], "the two choices");

  fire(keep[0], "click");
  await settle();
  const list = screen.kids.find(w => w.tag === "list");
  eq(list.kids.map(r => r.props.text), [
    "·  Write one in JSX",
    "·  Reorder without a rebuild",
    "·  Read docs/ui-runtime.md",
  ], "the done tasks were cleared");
  ok(!screen.kids.some(w => w.tag === "button"), "and the Clear button went with them");
});

it("the built tasks app starts from whatever is on the card", () => {
  const stored = JSON.stringify([{ id: 9, text: "From storage", done: false }]);
  const { screen } = runBuiltApp("app/apps/tasks.js", { "/tasks.json": stored });
  const list = screen.kids.find(w => w.tag === "list");
  eq(list.kids.map(r => r.props.text), ["·  From storage"], "rows from the stored file");
});

// ---------------------------------------------------------------- run

for (const [name, fn] of tests) await test(name, fn);

if (failed) {
  console.error("");
  for (const [name, e] of failures) {
    console.error(`FAIL  ${name}`);
    console.error("      " + String(e.message).split("\n").join("\n      "));
  }
  console.error(`\n${failed} failed, ${passed} passed`);
  process.exit(1);
}
console.log(`ok — ${passed} reconciler tests passed`);
