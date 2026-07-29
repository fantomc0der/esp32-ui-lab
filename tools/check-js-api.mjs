// check-js-api.mjs — verify the scripts only call bindings that exist.
//
// The selftest in app/selftest.js is the real check, but it needs the board.
// This is the part that can run without hardware: it reads the names the C
// layer actually registers and compares them against what the scripts call,
// so a typo or a binding removed from C fails the build instead of failing
// on the device.
//
//   node tools/check-js-api.mjs
//
// Namespaced calls only (lv.x, sys.x, fs.x, wifi.x, fetch). Widget methods are
// not checked: `.set(` on a JS array is indistinguishable from `.set(` on a
// widget without type inference, and guessing would mean false alarms.

import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";

const SRC = "firmware/lvgl-js-bindings/src";
const SCRIPTS = "app";

const cFiles = readdirSync(SRC)
  .filter(f => f.endsWith(".cpp"))
  .map(f => [f, readFileSync(join(SRC, f), "utf8")]);

const cSource = cFiles.map(([, text]) => text).join("\n");

// JS_SetPropertyStr(ctx, <target>, "<name>", ...) — the one call that publishes
// anything to scripts, so collecting it by target gives the whole surface.
//
// Collected per file, because the target is a C local whose name is not unique
// across the layer: several modules build their object in a variable called `o`.
// Merging those would let fs.<a sys or wifi key> pass this check.
const registered = {};      // target -> Set(name), merged across files
const perFile = {};         // file -> { target -> Set(name) }
for (const [file, text] of cFiles) {
  perFile[file] = {};
  for (const m of text.matchAll(/JS_SetPropertyStr\(\s*ctx,\s*(\w+),\s*"([^"]+)"/g)) {
    (registered[m[1]] ??= new Set()).add(m[2]);
    (perFile[file][m[1]] ??= new Set()).add(m[2]);
  }
}

// Widget constructors come from a table rather than individual calls.
const makers = new Set();
const makersBlock = cSource.match(/kMakers\[\]\s*=\s*\{([\s\S]*?)\};/);
if (makersBlock) {
  for (const m of makersBlock[1].matchAll(/"(\w+)"/g)) makers.add(m[1]);
}

const surface = {
  lv: new Set([...(registered.lv ?? []), ...makers]),
  sys: registered.sys ?? new Set(),
  // The fs module builds its object in a local called `o`, so it has to be read
  // from its own file only — `o` is used the same way in bindings_sys.cpp and
  // bindings_wifi.cpp for the objects sys.info() and wifi.status() return.
  fs: perFile["bindings_fs.cpp"]?.o ?? new Set(),
  wifi: registered.wifi ?? new Set(),
};
const globals = registered.global ?? new Set();

for (const [ns, names] of Object.entries(surface)) {
  if (names.size === 0) {
    console.error(`error: found no bindings for "${ns}" — did the C layer move?`);
    process.exit(2);
  }
}

// ---------------------------------------------------------------- scripts

function* scripts(dir) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) yield* scripts(p);
    else if (e.name.endsWith(".js")) yield p;
  }
}

let problems = 0;

// The component runtime reaches its widgets through lv[tag](), which the scan
// below cannot see, so its tag table is checked against the makers directly.
// Without this, dropping a maker from C would break every JSX app using that
// element with nothing failing until the panel showed it.
const RUNTIME = "app/lib/ui.js";
try {
  const text = readFileSync(RUNTIME, "utf8");
  const block = text.match(/HOST_TAGS\s*=\s*\[([\s\S]*?)\]/);
  if (!block) {
    console.error(`${RUNTIME}  HOST_TAGS is gone — this check can no longer see the element list`);
    problems++;
  } else {
    for (const m of block[1].matchAll(/"(\w+)"/g)) {
      if (!surface.lv.has(m[1])) {
        console.error(`${RUNTIME}  <${m[1]}> has no lv.${m[1]}() maker behind it`);
        problems++;
      }
    }
  }
} catch {
  // No runtime in the tree is fine; it is an app-layer library, not firmware.
}

for (const file of scripts(SCRIPTS)) {
  const text = readFileSync(file, "utf8");

  for (const m of text.matchAll(/\b(lv|sys|fs|wifi)\.(\w+)\s*\(/g)) {
    const [, ns, name] = m;
    if (!surface[ns].has(name)) {
      const line = text.slice(0, m.index).split("\n").length;
      console.error(`${file}:${line}  ${ns}.${name}() is not a binding`);
      problems++;
    }
  }

  if (/\bfetch\s*\(/.test(text) && !globals.has("fetch")) {
    console.error(`${file}  calls fetch() but it is not registered`);
    problems++;
  }
}

const total = Object.values(surface).reduce((n, s) => n + s.size, 0);
if (problems) {
  console.error(`\n${problems} problem(s) against ${total} known bindings`);
  process.exit(1);
}
console.log(`ok — scripts only use the ${total} bindings the C layer registers`);
