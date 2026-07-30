// selftest.js — exercises the binding surface and prints PASS/FAIL over serial.
//
// Deploy it the same way as app.js (SD card, or app-begin/app-end over serial)
// and watch the monitor at 115200. Ends with a SELFTEST line giving the count;
// anything other than "0 failed" means the binding layer regressed.
//
// Includes regression checks for two use-after-free bugs found on hardware:
// a timer callback stopping its own timer, and a widget handle used after its
// container was cleaned. Both crashed or silently corrupted memory once.
//
// Touch-driven behaviour cannot be covered here (no way to synthesize a tap),
// so event coverage stops at registration and argument validation.

let pass = 0, fail = 0;

function check(name, fn) {
  let ok;
  try {
    ok = fn();
  } catch (e) {
    fail++;
    console.log(`FAIL ${name} — threw ${e.message}`);
    return;
  }
  if (ok === true) {
    pass++;
    console.log(`PASS ${name}`);
  } else {
    fail++;
    console.log(`FAIL ${name} — got ${ok}`);
  }
}

// True when fn() throws, which is the expected outcome for misuse.
function threw(fn) {
  try { fn(); return false; } catch (e) { return true; }
}

const scr = lv.screen().set({ bg: "#101418" });
lv.label(scr, { align: "top-mid", y: 4, font: 16, text: "selftest running" });

// ---------------------------------------------------------------- widgets
const panel = lv.obj(scr, { w: 200, h: 60, align: "center", pad: 4, scroll: false });

check("obj creates and returns a handle", () => typeof panel === "object");
check("set() is chainable", () => panel.set({ radius: 4 }) === panel);
check("label accepts text", () => typeof lv.label(panel, { text: "hi" }) === "object");
check("button accepts text", () => typeof lv.button(panel, { text: "b", w: 40, h: 20 }) === "object");

const slider = lv.slider(panel, { w: 100, h: 10, range: [0, 50], value: 25 });
check("slider stores its value", () => slider.value() === 25);
check("value() setter is chainable", () => slider.value(30) === slider);
check("slider round-trips a new value", () => slider.value() === 30);

const sw = lv.switch(panel, { value: true });
check("switch reads back true", () => sw.value() === true);
check("switch reads back false", () => { sw.value(false); return sw.value() === false; });

const arc = lv.arc(panel, { range: [0, 10], value: 7, knob: false });
check("arc stores its value", () => arc.value() === 7);

const chart = lv.chart(panel, { w: 60, h: 30, points: 8, range: [0, 100] });
check("chart.push is chainable", () => chart.push(42) === chart);

const list = lv.list(panel, { w: 80, h: 40 });
check("list.add returns a row handle", () => typeof list.add("row") === "object");

const tabs = lv.tabview(scr, { bar: 20, w: 100, h: 40, align: "bottom-mid" });
check("tabview.addTab returns a container", () => typeof tabs.addTab("t1") === "object");

check("bounds() reports geometry", () => {
  const b = panel.bounds();
  return b.w > 0 && b.h > 0 && typeof b.x === "number" && typeof b.y === "number";
});

// lv.size() is what lets a script lay out for the panel it is on rather than the
// one it was written for, so it has to report the real display, not zeros.
check("size() reports the panel", () => {
  const s = lv.size();
  return s.w > 0 && s.h > 0;
});
check("size() agrees with the screen's own bounds", () => {
  const b = scr.bounds(), s = lv.size();
  // The screen's content area is the panel less its padding, so it cannot exceed
  // the display and should not be wildly smaller either.
  return b.w <= s.w && b.h <= s.h && b.w > s.w - 40 && b.h > s.h - 40;
});

// Resolution independence: a percentage must resolve against the parent's
// content area, not be mistaken for "content" sizing.
check("percentage width resolves against the parent", () => {
  const outer = lv.obj(scr, { w: 100, h: 40, pad: 0, border: 0, scroll: false });
  const half = lv.obj(outer, { w: "50%", h: 10, pad: 0, border: 0 });
  const got = half.bounds().w;
  outer.clean();
  return got > 40 && got < 60;  // ~50 of the parent's 100
});

check("flex row lays children out left to right", () => {
  const bar = lv.obj(scr, { w: 120, h: 30, pad: 0, border: 0, scroll: false, flex: "row" });
  const a = lv.label(bar, { text: "a" });
  const b = lv.label(bar, { text: "b" });
  const ax = a.bounds().x, bx = b.bounds().x;
  bar.clean();
  return bx > ax;
});

check("flex column stacks children downward", () => {
  const col = lv.obj(scr, { w: 60, h: 60, pad: 0, border: 0, scroll: false, flex: "column" });
  const a = lv.label(col, { text: "a" });
  const b = lv.label(col, { text: "b" });
  const ay = a.bounds().y, by = b.bounds().y;
  col.clean();
  return by > ay;
});

check("flexAlign accepts a pair", () => {
  const box = lv.obj(scr, { w: 60, h: 30, flex: "row", flexAlign: ["center", "center"] });
  lv.label(box, { text: "c" });
  box.clean();
  return true;
});

// ---------------------------------------------------------------- forms and status
const forms = lv.obj(scr, { w: 120, h: 60, hidden: true, scroll: false });

const bar = lv.bar(forms, { w: 100, h: 10, range: [0, 200], value: 150 });
check("bar stores its value", () => bar.value() === 150);
check("bar clamps to its range", () => { bar.value(500); return bar.value() === 200; });

const cb = lv.checkbox(forms, { text: "Enable", value: true });
check("checkbox reads back true", () => cb.value() === true);
check("checkbox reads back false", () => { cb.value(false); return cb.value() === false; });

const roller = lv.roller(forms, { options: ["red", "green", "blue"], value: 1 });
// The trap this covers: apply_props reads `value` before it reads `options`,
// so a selection passed alongside the list it indexes into would land on 0
// unless it is applied a second time.
check("roller takes options and a selection in one call", () => roller.value() === 1);
check("roller selection round-trips", () => { roller.value(2); return roller.value() === 2; });
check("roller accepts a newline string too", () => {
  const r = lv.roller(forms, { options: "a\nb\nc", value: 2 });
  const ok = r.value() === 2;
  r.delete();
  return ok;
});

const dd = lv.dropdown(forms, { options: ["one", "two", "three"], value: 2 });
check("dropdown takes options and a selection in one call", () => dd.value() === 2);
check("dropdown selection round-trips", () => { dd.value(0); return dd.value() === 0; });

check("spinner creates and takes its timings", () => {
  const sp = lv.spinner(forms, { w: 20, h: 20, duration: 800, sweep: 60 });
  const ok = typeof sp === "object";
  sp.delete();
  return ok;
});

const led = lv.led(forms, { w: 12, h: 12, color: "#4CAF50", value: true });
check("led reads back on", () => led.value() === true);
check("led reads back off", () => { led.value(false); return led.value() === false; });
check("led accepts a brightness", () => led.set({ value: true, brightness: 120 }) === led);

// ---------------------------------------------------------------- delete and index
//
// The two the component runtime is built on: removing one child of a container
// while its siblings stay, and reordering without rebuilding.
check("delete() removes one child, leaving its siblings", () => {
  const box = lv.obj(scr, { w: 60, h: 40, hidden: true });
  const a = lv.label(box, { text: "a" });
  const b = lv.label(box, { text: "b" });
  const c = lv.label(box, { text: "c" });
  b.delete();
  const ok = threw(() => b.set({ text: "gone" })) &&
             a.index() === 0 && c.index() === 1;
  box.clean();
  return ok;
});

check("index() reports position among siblings", () => {
  const box = lv.obj(scr, { w: 60, h: 40, hidden: true });
  const a = lv.label(box, { text: "a" });
  const b = lv.label(box, { text: "b" });
  const ok = a.index() === 0 && b.index() === 1;
  box.clean();
  return ok;
});

check("index(n) moves a widget and is chainable", () => {
  const box = lv.obj(scr, { w: 60, h: 40, hidden: true });
  const a = lv.label(box, { text: "a" });
  const b = lv.label(box, { text: "b" });
  const c = lv.label(box, { text: "c" });
  const ok = c.index(0) === c && c.index() === 0 && a.index() === 1 && b.index() === 2;
  box.clean();
  return ok;
});

// A deleted widget's event binding must be released by the LV_EVENT_DELETE
// hook, the same path .clean() uses. Nothing here can observe the free
// directly; what it can observe is that deleting a widget with a handler
// attached does not fault.
check("delete() releases an event binding", () => {
  const box = lv.obj(scr, { w: 40, h: 20, hidden: true });
  const btn = lv.button(box, { w: 30, h: 16, text: "x" });
  btn.on("click", () => console.log("never"));
  btn.delete();
  box.clean();
  return true;
});

// ---------------------------------------------------------------- misuse is rejected
//
// The marked lines call a method on a widget that does not have it, which is
// the point: only the board can prove the C guard throws. Without the marker
// tools/check-js-api.mjs reports them as the mistake they are imitating.
check("delete() refuses the screen", () => threw(() => scr.delete()));
check("push() rejects a non-chart", () => threw(() => panel.push(1)));          // check-js-api: wrong kind on purpose
check("addTab() rejects a non-tabview", () => threw(() => panel.addTab("x")));  // check-js-api: wrong kind on purpose
check("add() rejects a non-list", () => threw(() => panel.add("x")));           // check-js-api: wrong kind on purpose
check("on() rejects a non-function", () => threw(() => panel.on("click", 42)));
check("on() rejects an unknown event", () => threw(() => panel.on("teleport", () => {})));
check("timer() rejects a non-function", () => threw(() => lv.timer(50, "nope")));
check("unknown props are ignored, not fatal", () => panel.set({ notAProp: 1 }) === panel);

// REGRESSION: a handle whose container was cleaned must throw, never write
// into freed memory (it used to silently succeed).
check("stale handle throws after clean()", () => {
  const doomed = lv.obj(scr, { w: 10, h: 10 });
  const kid = lv.label(doomed, { text: "k" });
  doomed.clean();
  return threw(() => kid.set({ text: "boom" })) && threw(() => kid.value());
});

// ---------------------------------------------------------------- text input
check("textarea round-trips its text", () => {
  const ta = lv.textarea(scr, { w: 80, h: 30, oneLine: true, hidden: true });
  ta.value("hello");
  const got = ta.value();
  ta.set({ hidden: true });
  return got === "hello";
});

check("keyboard targets a textarea", () => {
  const ta = lv.textarea(scr, { w: 80, h: 30, hidden: true });
  const kb = lv.keyboard(scr, { w: 100, h: 60, hidden: true });
  return kb.target(ta) === kb;
});

check("target() rejects a non-keyboard", () => threw(() => panel.target(panel)));  // check-js-api: wrong kind on purpose

// ---------------------------------------------------------------- fs
check("fs reports availability", () => typeof fs.available() === "boolean");

check("fs write/read/remove round-trip", () => {
  const p = "/selftest-tmp.txt";
  if (!fs.available()) return true;           // nothing mounted; not a failure
  const wrote = fs.write(p, "abc");
  const back = fs.read(p);
  const gone = fs.remove(p) && !fs.exists(p);
  return wrote && back === "abc" && gone;
});

check("fs.append extends a file", () => {
  const p = "/selftest-tmp2.txt";
  if (!fs.available()) return true;
  fs.write(p, "a");
  fs.append(p, "b");
  const got = fs.read(p);
  fs.remove(p);
  return got === "ab";
});

check("fs.read of a missing file is null", () =>
  !fs.available() || fs.read("/definitely-not-here.txt") === null);

check("fs.list returns an array or null", () => {
  if (!fs.available()) return true;
  const l = fs.list("/");
  return l === null || Array.isArray(l);
});

check("relative paths are rejected", () => threw(() => fs.read("nope.txt")));

// ---------------------------------------------------------------- network
check("wifi.status has the expected shape", () => {
  const s = wifi.status();
  return typeof s.connected === "boolean" && typeof s.ssid === "string" &&
         typeof s.ip === "string" && typeof s.saved === "boolean";
});

check("wifi.status never leaks the password", () => {
  const s = wifi.status();
  return !("password" in s) && !("pass" in s);
});

check("fetch exists", () => typeof fetch === "function");

check("fetch without a connection fails fast", () => {
  if (wifi.status().connected) return true;   // can't test the guard when up
  return threw(() => fetch("http://example.com"));
});

// ---------------------------------------------------------------- app switching
check("sys.launch exists and defers", () => {
  // Calling it would switch apps out from under the test, so only check the
  // shape. The switch itself is exercised by the launcher.
  return typeof sys.launch === "function";
});

// ---------------------------------------------------------------- pinning
// Writes NVS, so put back whatever was pinned before the test ran.
const pinnedBefore = sys.pinned();

check("sys.pinned reports a path or null", () =>
  pinnedBefore === null || typeof pinnedBefore === "string");

check("pin/pinned/unpin round-trip", () => {
  const p = "/apps/selftest-pin.js";
  const set = sys.pin(p) && sys.pinned() === p;
  const cleared = sys.unpin() && sys.pinned() === null;
  return set && cleared;
});

check("pin() rejects a relative path", () => threw(() => sys.pin("apps/x.js")));
check("pin() rejects a missing argument", () => threw(() => sys.pin()));

if (pinnedBefore) sys.pin(pinnedBefore);
check("the pin the board had is back", () => sys.pinned() === pinnedBefore);

// ---------------------------------------------------------------- events
check("on() returns the widget", () => {
  const b = lv.button(panel, { text: "e", w: 20, h: 20 });
  return b.on("click", () => {}) === b;
});

check("on() accepts longpress", () => {
  const b = lv.button(panel, { text: "l", w: 20, h: 20 });
  return b.on("longpress", () => {}) === b;
});

// ---------------------------------------------------------------- sys / console
check("sys.heap reports both pools", () => {
  const h = sys.heap();
  return h.internal > 0 && h.psram > 0;
});
check("sys.info identifies the chip", () => sys.info().model.indexOf("ESP32") === 0);
check("sys.uptime advances", () => sys.uptime() > 0);
check("sys.fps returns a number", () => typeof sys.fps() === "number");
check("sys.battery is a number or null", () => {
  const b = sys.battery();
  return b === null || typeof b === "number";
});
check("sys.backlight accepts a percent", () => { sys.backlight(80); return true; });
// The panel is at 80 from the check above, and staying there is half of what
// these assert: each of these calls used to be read as "set it to 0" and dim it
// to the PWM floor, where it looks like a crashed board. Watching the panel not
// change is the eye-checkable half. TypeError is pinned rather than using
// threw(), because the error type is the part a script would branch on.
check("sys.backlight rejects a missing argument", () => {
  try { sys.backlight(); return false; } catch (e) { return e instanceof TypeError; }
});
check("sys.backlight rejects undefined", () => {
  try { sys.backlight(undefined); return false; } catch (e) { return e instanceof TypeError; }
});
// null is the one worth having on hardware: JSON has no undefined, so a config
// read off the card with a missing brightness arrives here as null, and JS would
// convert it to 0.
check("sys.backlight rejects null", () => {
  try { sys.backlight(null); return false; } catch (e) { return e instanceof TypeError; }
});
check("sys.backlight rejects a string", () => {
  try { sys.backlight("80"); return false; } catch (e) { return e instanceof TypeError; }
});
check("sys.backlight still accepts 0 deliberately", () => {
  sys.backlight(0);
  sys.backlight(80);
  return true;
});
check("console.log accepts many args", () => { console.log("  (console ok)", 1, true); return true; });

// ---------------------------------------------------------------- async
let asyncPending = 3;
const asyncDone = () => { if (--asyncPending === 0) summarize(); };

// REGRESSION: the one-shot idiom. Stopping your own timer from inside its
// callback used to free the running closure and panic the board.
let oneshotRan = 0;
const oneshot = lv.timer(120, () => {
  oneshotRan++;
  oneshot.stop();
  check("timer callback survives stopping itself", () => true);
  check("stop() twice is a harmless no-op", () => { oneshot.stop(); return true; });
  asyncDone();
});

// A repeating timer must actually repeat, then stop on request.
let ticks = 0;
const repeater = lv.timer(60, () => {
  if (++ticks >= 3) {
    repeater.stop();
    check("repeating timer fired 3 times", () => ticks === 3);
    asyncDone();
  }
});

// Promises only resolve if the host pumps QuickJS's job queue.
Promise.resolve(21)
  .then(v => v * 2)
  .then(v => {
    check("promise chain resolves (job pump alive)", () => v === 42);
    return (async () => "awaited")();
  })
  .then(v => {
    check("async/await resolves", () => v === "awaited");
    asyncDone();
  });

function summarize() {
  console.log(`SELFTEST ${pass} passed, ${fail} failed`);
  lv.label(scr, {
    align: "bottom-mid", y: -2, font: 16,
    color: fail === 0 ? "#4CAF50" : "#FF5252",
    text: fail === 0 ? `all ${pass} checks passed` : `${fail} of ${pass + fail} FAILED`,
  });
}

console.log("selftest: synchronous checks done, waiting on timers/promises");
