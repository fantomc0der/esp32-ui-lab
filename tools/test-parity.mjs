// test-parity.mjs — does each ported app build the same screen as the original?
//
//   node tools/test-parity.mjs
//
// Every app in app/apps was rewritten from imperative `lv` calls to JSX
// components. "It still looks right on the panel" is a weak claim to make about
// that, and an unverifiable one in CI, so this makes the strong one instead:
// run both versions against the same fake `lv`, with the same frozen sensor
// readings, storage and network, and compare the widget trees they produce —
// every widget, its parent, and every prop that reaches it.
//
// The originals are frozen fixtures rather than git revisions. Reading them out
// of history looked tidier, but CI checks out at depth 1 and a squash-merge
// would orphan the commit, so the baseline has to be a file.
//
// What this does and does not prove. It proves the two build the same widgets
// with the same props, given the same inputs and the same sequence of external
// events. It cannot prove anything touch-driven, because a tap cannot be
// synthesized here any more than it can in app/selftest.js — the deeper screens
// of the wifi app, the launcher's pin gestures and the dashboard's touch dot
// all still need a finger on the panel.

import { readFileSync } from "node:fs";
import { makeLv, makeFs, makeSys, makeWifi } from "./lv-mock.mjs";

const settle = () => new Promise(r => setImmediate(r));

// Every widget as one line: where it sits in the tree, what it is, and every
// prop that was applied to it. Property order is normalised so two apps that
// set the same things in a different sequence still compare equal — the order
// of `.set()` calls is not something a port has to preserve.
function describe(node, path = "", out = []) {
  let i = 0;
  for (const kid of node.kids) {
    const at = `${path}/${kid.tag}[${i++}]`;
    const props = Object.keys(kid.props).sort()
      .map(k => `${k}=${JSON.stringify(kid.props[k])}`)
      .join(" ");
    out.push(`${at}  ${props}`);
    describe(kid, at, out);
  }
  return out;
}

// Find a widget by the text on it. A button's text lives on the label the C
// layer grows for it, so both places are checked.
function findByText(node, text, out = []) {
  for (const k of node.kids) {
    const own = k.props.text;
    const inner = k.kids.find(c => c.synthetic);
    if (own === text || (inner && inner.props.text === text)) out.push(k);
    findByText(k, text, out);
  }
  return out;
}

// Tap something, then let both versions get where they are going. The
// imperative app defers a screen rebuild through an lv.timer(20), so it needs a
// tick; the port defers through a microtask, so it needs a settle. Doing both
// is what lets one drive script exercise the two.
async function tap(env, text, nth = 0) {
  const hit = findByText(env.screen, text)[nth];
  if (!hit) throw new Error(`nothing on screen says "${text}"`);
  env.fire(hit, "click");
  env.tick();
  await settle();
  await settle();
}

// ---------------------------------------------------------------- the apps

const WEATHER_BODY = JSON.stringify({
  // Values chosen so round() and floor() disagree, and so the wind
  // formatting has something to lose: 18.6 -> "19", 11.7 -> "12".
  current: { temperature_2m: 18.6, weather_code: 3, wind_speed_10m: 11.7 },
});

const apps = [
  {
    name: "vitals",
    original: "tools/fixtures/vitals-imperative.js",
    ported: "app/apps/vitals.js",
    // One readout cycle, a wifi scan that completes, then another readout.
    async drive(env) {
      env.tick(); await settle();
      env.wifi.deliver(); await settle();
      env.tick(); await settle();
    },
  },
  // Three runs, because an app's states are as much a part of "behaves the
  // same" as its layout, and a single happy path exercises one of weather's
  // five. A mutation to the cached colour went undetected until these existed.
  {
    name: "weather",
    original: "tools/fixtures/weather-imperative.js",
    ported: "app/apps/weather.js",
    files: { "/config/weather.json": JSON.stringify({ name: "Testville", lat: 1.5, lon: -2.5 }) },
    // The fetch resolves on its own; both versions just need the microtasks run.
    async drive() { await settle(); await settle(); await settle(); },
  },
  {
    name: "weather/offline",
    original: "tools/fixtures/weather-imperative.js",
    ported: "app/apps/weather.js",
    // A cache on the card and no radio: the screen must show the old reading
    // rather than dashes, and say it is stale.
    files: {
      "/config/weather.json": JSON.stringify({ name: "Testville", lat: 1.5, lon: -2.5 }),
      "/cache/weather.json": JSON.stringify({ temp: 7.6, code: 61, wind: 4.8 }),
    },
    connected: false,
    async drive() { await settle(); await settle(); },
  },
  {
    name: "weather/failed",
    original: "tools/fixtures/weather-imperative.js",
    ported: "app/apps/weather.js",
    files: { "/config/weather.json": JSON.stringify({ name: "Testville", lat: 1.5, lon: -2.5 }) },
    fetchFails: true,
    async drive() { await settle(); await settle(); await settle(); },
  },
  {
    name: "launcher",
    original: "tools/fixtures/launcher-imperative.js",
    ported: "app/app.js",
    dirs: { "/apps": ["vitals.js", "weather.js", "wifi.js"] },
    files: { "/apps/vitals.js": "", "/apps/weather.js": "", "/apps/wifi.js": "" },
    pinned: "/apps/weather.js",
    async drive() { await settle(); },
  },
  {
    name: "wifi",
    original: "tools/fixtures/wifi-imperative.js",
    ported: "app/apps/wifi.js",
    // The status screen polls every 1.5 s; both versions must land on the same
    // thing after a tick.
    async drive(env) { env.tick(); await settle(); },
  },
  // The wifi app is four screens, and the status one is the only one you reach
  // without tapping. A tap cannot be synthesized on the panel, but it can be
  // here, so the deeper screens get compared too — which is where the six
  // next() deferrals the port deleted used to live.
  {
    name: "wifi/scan",
    original: "tools/fixtures/wifi-imperative.js",
    ported: "app/apps/wifi.js",
    async drive(env) {
      await tap(env, "Scan");
      env.wifi.deliver();
      await settle();
      await settle();
    },
  },
  {
    name: "wifi/password",
    original: "tools/fixtures/wifi-imperative.js",
    ported: "app/apps/wifi.js",
    async drive(env) {
      await tap(env, "Scan");
      env.wifi.deliver();
      await settle();
      await settle();
      // The strongest network in the fixture, which is closed, so this is the
      // password prompt rather than a straight join.
      await tap(env, "home   ||| *");
    },
  },
];

async function run(app, source, label) {
  const env = makeLv();
  const fs = makeFs({ ...(app.files || {}) }, { ...(app.dirs || {}) });
  const wifi = makeWifi();
  if (app.connected === false) {
    wifi.status = () => ({ connected: false, ssid: "", ip: "", rssi: 0, saved: true });
  }
  const sys = makeSys({ pinned: () => app.pinned ?? null });
  const errors = [];
  const log = { log: () => {}, error: (...a) => errors.push(a.join(" ")) };
  const fetchMock = () => (app.fetchFails
    ? Promise.reject(new Error("no route to host"))
    : Promise.resolve({ status: 200, ok: true, body: WEATHER_BODY }));

  new Function("lv", "sys", "fs", "wifi", "fetch", "console", source)(
    env.lv, sys, fs, wifi, fetchMock, log);
  await settle();
  await app.drive({ ...env, wifi });

  if (errors.length) throw new Error(`${app.name}: ${label} reported errors:\n  ${errors.join("\n  ")}`);
  return { lines: describe(env.screen), stats: env.stats };
}

// ---------------------------------------------------------------- compare

let failed = 0;
for (const app of apps) {
  let a, b;
  try {
    a = await run(app, readFileSync(app.original, "utf8"), "the imperative original");
    b = await run(app, readFileSync(app.ported, "utf8"), "the JSX port");
  } catch (e) {
    console.error(`FAIL ${app.name}: ${e.message}`);
    failed++;
    continue;
  }

  const diffs = [];
  const max = Math.max(a.lines.length, b.lines.length);
  for (let i = 0; i < max; i++) {
    if (a.lines[i] !== b.lines[i]) diffs.push([i, a.lines[i], b.lines[i]]);
  }

  if (!diffs.length) {
    console.log(`ok   ${app.name.padEnd(9)} same ${max} widgets, same props ` +
                `(${a.stats.sets} .set() imperative, ${b.stats.sets} jsx)`);
    continue;
  }

  failed++;
  console.error(`FAIL ${app.name}: the port builds a different screen ` +
                `(${diffs.length} of ${max} widgets differ)`);
  for (const [i, was, now] of diffs.slice(0, 12)) {
    console.error(`  #${i}`);
    console.error(`    imperative: ${was ?? "(nothing)"}`);
    console.error(`    jsx:        ${now ?? "(nothing)"}`);
  }
  if (diffs.length > 12) console.error(`  ... and ${diffs.length - 12} more`);
}

if (failed) {
  console.error(`\n${failed} of ${apps.length} ports do not match their originals`);
  process.exit(1);
}
console.log(`\nok — all ${apps.length} ports build what they replaced`);
