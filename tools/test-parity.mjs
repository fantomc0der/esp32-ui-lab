// test-parity.mjs — does the ported app build the same screen as the original?
//
//   node tools/test-parity.mjs
//
// apps/vitals.js was rewritten from imperative `lv` calls to JSX components
// (app/src/vitals.jsx). "It still looks right on the panel" is a weak claim to
// make about that, and an unverifiable one in CI, so this makes the strong one
// instead: run both versions against the same fake `lv`, with the same frozen
// sensor readings, and compare the widget trees they produce — every widget,
// its parent, and every prop that reaches it.
//
// The imperative original is kept as a frozen fixture rather than read out of
// git history. Reading it from git looked tidier, but CI checks out at depth 1
// and a squash-merge would orphan the commit, so the baseline has to be a file.
//
// What this does and does not prove. It proves the two build the same widgets
// with the same props, that both survive a readout cycle and a wifi scan, and
// that the JSX version's two-pass arc measurement lands on the number the
// imperative one computed in one pass. It cannot prove anything touch-driven,
// because a tap cannot be synthesized here any more than it can in
// app/selftest.js — tab switching, the touch dot and the backlight slider still
// need a finger on the panel.

import { readFileSync } from "node:fs";
import { makeLv, makeSys, makeWifi } from "./lv-mock.mjs";

const ORIGINAL = "tools/fixtures/vitals-imperative.js";   // frozen, pre-port
const PORTED = "app/apps/vitals.js";                      // built from app/src/vitals.jsx

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

async function run(source, label) {
  const env = makeLv();
  const sys = makeSys();
  const wifi = makeWifi();
  const errors = [];
  const log = { log: () => {}, error: (...a) => errors.push(a.join(" ")) };

  new Function("lv", "sys", "wifi", "console", source)(env.lv, sys, wifi, log);
  await settle();

  // One readout cycle, then a scan that completes. Both apps drive these the
  // same way from the outside, which is the point: the test pokes the bindings,
  // not the app.
  env.tick();
  await settle();
  wifi.deliver();
  await settle();
  env.tick();
  await settle();

  if (errors.length) throw new Error(`${label} reported errors:\n  ${errors.join("\n  ")}`);
  return { lines: describe(env.screen), stats: env.stats };
}

const original = readFileSync(ORIGINAL, "utf8");
const ported = readFileSync(PORTED, "utf8");

const a = await run(original, "the imperative original");
const b = await run(ported, "the JSX port");

const diffs = [];
const max = Math.max(a.lines.length, b.lines.length);
for (let i = 0; i < max; i++) {
  if (a.lines[i] !== b.lines[i]) diffs.push([i, a.lines[i], b.lines[i]]);
}

if (diffs.length) {
  console.error(`vitals: the port builds a different screen (${diffs.length} of ${max} widgets differ)\n`);
  for (const [i, was, now] of diffs.slice(0, 25)) {
    console.error(`  #${i}`);
    console.error(`    imperative: ${was ?? "(nothing)"}`);
    console.error(`    jsx:        ${now ?? "(nothing)"}`);
  }
  if (diffs.length > 25) console.error(`  ... and ${diffs.length - 25} more`);
  process.exit(1);
}

console.log(`ok — the JSX port of vitals builds the same ${max} widgets, with the same props`);
console.log(`     imperative: ${a.stats.created} widgets created, ${a.stats.sets} .set() calls`);
console.log(`     jsx:        ${b.stats.created} widgets created, ${b.stats.sets} .set() calls`);
