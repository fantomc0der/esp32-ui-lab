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

// ---------------------------------------------------------------- misuse is rejected
check("push() rejects a non-chart", () => threw(() => panel.push(1)));
check("addTab() rejects a non-tabview", () => threw(() => panel.addTab("x")));
check("add() rejects a non-list", () => threw(() => panel.add("x")));
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

// ---------------------------------------------------------------- events
check("on() returns the widget", () => {
  const b = lv.button(panel, { text: "e", w: 20, h: 20 });
  return b.on("click", () => {}) === b;
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
