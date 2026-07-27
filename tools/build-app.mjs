// build-app.mjs — turn app/src/*.jsx into the plain .js the board loads.
//
//   node tools/build-app.mjs                 build every app/src/*.jsx
//   node tools/build-app.mjs counter         build one
//   node tools/build-app.mjs --check         fail if a committed output is stale
//
// The board has no module system and loads one file, so a built app is the
// runtime plus the app, concatenated. The output is committed, because the
// output is what ships: app/apps/*.js is the card layout, and push.ps1 sends
// from there.
//
// Two things this does that a plain concatenation would not:
//
//   The runtime is stripped of comments and reflowed onto long lines. That is
//   not for bytes, it is for the serial path: push.ps1 sleeps between lines and
//   the host reads one character per loop(), so line count is what a push
//   costs. It also keeps the app's own line numbers close to the .jsx, so a
//   stack trace off the board still points somewhere useful.
//
//   Lines stay well under the host's 4 KB cap (jsvm_app.cpp), past which a
//   pushed line is silently truncated into a file that still parses.

import { readFileSync, writeFileSync, readdirSync, existsSync, mkdirSync } from "node:fs";
import { join, basename, sep } from "node:path";
import { execFileSync } from "node:child_process";
import { transformJsx } from "./jsx.mjs";

const SRC = "app/src";
const OUT = "app/apps";
const RUNTIME = "app/lib/ui.js";

// Long enough that the runtime is a couple of dozen lines, short enough that
// one line is well under both the host's 4 KB line cap and what the USB CDC
// FIFO holds between two loop() iterations.
const LINE_WIDTH = 180;

// What the runtime hands back; the bundle destructures exactly these.
const EXPORTS = ["h", "Fragment", "render", "useState", "useEffect", "useRef",
                 "useMemo", "useCallback", "useInterval"];

// ---------------------------------------------------------------- packing

// Drops comments while leaving strings, template literals and regex literals
// alone. Same `/` ambiguity as the JSX scanner, resolved the same way: a slash
// in an expression position starts a regex, anywhere else it is division.
function stripComments(src) {
  let out = "";
  let i = 0;
  let last = "";
  const EXPR_AFTER = "([{,;:=+-*/%!&|^~?<>\n";
  const KEYWORDS = /\b(return|case|in|of|typeof|instanceof|new|delete|void|do|else|yield|await|throw)$/;

  const exprPos = () => last === "" || EXPR_AFTER.includes(last) || KEYWORDS.test(out);

  while (i < src.length) {
    const c = src[i];
    if (c === "/" && src[i + 1] === "/") {
      while (i < src.length && src[i] !== "\n") i++;
      continue;
    }
    if (c === "/" && src[i + 1] === "*") {
      i += 2;
      while (i < src.length && !(src[i] === "*" && src[i + 1] === "/")) i++;
      i += 2;
      continue;
    }
    if (c === '"' || c === "'" || c === "`") {
      const q = c;
      out += src[i++];
      while (i < src.length) {
        const d = src[i];
        out += d;
        i++;
        if (d === "\\") { out += src[i]; i++; continue; }
        if (d === q) break;
      }
      last = "x";
      continue;
    }
    if (c === "/" && exprPos()) {
      out += src[i++];
      let inClass = false;
      while (i < src.length) {
        const d = src[i];
        out += d;
        i++;
        if (d === "\\") { out += src[i]; i++; continue; }
        if (d === "[") inClass = true;
        else if (d === "]") inClass = false;
        else if (d === "/" && !inClass) break;
      }
      last = "x";
      continue;
    }
    out += c;
    if (!/\s/.test(c) || c === "\n") last = c;
    i++;
  }
  return out;
}

// Every statement in the runtime ends in `;` or `}`, so joining lines with a
// space is always valid — there is nothing here that relies on a newline to
// terminate it. The build verifies that claim with `node --check` rather than
// trusting it.
function reflow(src) {
  const lines = src.split("\n").map(l => l.trim()).filter(Boolean);
  const out = [];
  let cur = "";
  for (const line of lines) {
    if (!cur) { cur = line; continue; }
    if (cur.length + 1 + line.length > LINE_WIDTH) { out.push(cur); cur = line; }
    else cur += " " + line;
  }
  if (cur) out.push(cur);
  return out.join("\n");
}

// The app goes first, wrapped in a function, and the runtime after it. That
// ordering is the whole reason for the wrapper: the runtime is a couple of
// hundred lines even reflowed (it cannot be one, the host truncates a line past
// 4 KB), so putting it first would push every line of the app down by that much
// and a stack trace off the board would point nowhere near the .jsx. This way
// the offset is three lines, whatever the runtime grows to.
//
// The wrapper also keeps the app's top-level names to itself, which the runtime
// already does. The cost is that the serial REPL can no longer reach into an
// app's variables — true of the runtime's internals either way, and the reason
// a built app is a build artifact rather than something to debug in place.
function bundle(appSource, appName) {
  const runtime = reflow(stripComments(readFileSync(RUNTIME, "utf8")));
  return [
    `// GENERATED by tools/build-app.mjs from app/src/${appName}.jsx — do not edit.`,
    `// Edit the source and rebuild:  node tools/build-app.mjs ${appName}`,
    `function __app() {   // line ${BANNER_LINES + 1} here is line 1 of the .jsx`,
    appSource.replace(/\n$/, ""),
    `}`,
    `const {${EXPORTS.join(", ")}} = (function () {`,
    runtime,
    `return UI; })();`,
    `__app();`,
    "",
  ].join("\n");
}

const BANNER_LINES = 3;

// ---------------------------------------------------------------- driver

// A source builds to app/apps/<name>.js, the card's app directory, unless it
// says otherwise with `// @out <path>` near the top. The launcher lists
// everything in /apps, so a test belongs outside it — the same reason
// app/selftest.js is not in there.
function buildOne(name) {
  const srcPath = join(SRC, name + ".jsx");
  const jsx = readFileSync(srcPath, "utf8");
  const directive = jsx.slice(0, 2000).match(/^\s*\/\/\s*@out\s+(\S+)\s*$/m);
  const outPath = directive ? directive[1].split("/").join(sep) : join(OUT, name + ".js");
  return { outPath, text: bundle(transformJsx(jsx, srcPath), name) };
}

// Control characters are the one thing `node --check` will not catch for us.
// Node accepts a raw NUL inside a string literal; QuickJS's tokenizer rejects
// it, so the file parses on the PC, survives the checksummed push intact, and
// then fails to evaluate on the panel with a syntax error pointing at a line
// that looks perfectly fine. Found exactly that way, once.
function checkCharacters(path, text) {
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    if (c > 31 || c === 9 || c === 10 || c === 13) continue;
    const line = text.slice(0, i).split("\n").length;
    const col = i - text.lastIndexOf("\n", i - 1);
    throw new Error(
      `${path}:${line}:${col}: control character U+${c.toString(16).padStart(4, "0")} in the source.\n` +
      `  Node accepts it, QuickJS does not — this would fail only on the board.`);
  }
}

function checkSyntax(path, text) {
  const tmp = join(process.env.TEMP || "/tmp", "build-app-check.js");
  writeFileSync(tmp, text);
  try {
    execFileSync(process.execPath, ["--check", tmp], { stdio: "pipe" });
  } catch (e) {
    const msg = (e.stderr || "").toString().split("\n").slice(0, 6).join("\n");
    throw new Error(`${path} does not parse after the transform:\n${msg}`);
  }
}

const args = process.argv.slice(2);
const check = args.includes("--check");
const names = args.filter(a => !a.startsWith("--"));

if (!existsSync(SRC)) {
  console.error(`no ${SRC}/ — nothing to build`);
  process.exit(0);
}
if (!existsSync(OUT)) mkdirSync(OUT, { recursive: true });

const targets = names.length
  ? names.map(n => basename(n).replace(/\.jsx?$/, ""))
  : readdirSync(SRC).filter(f => f.endsWith(".jsx")).map(f => f.slice(0, -4));

let stale = 0;
for (const name of targets) {
  const { outPath, text } = buildOne(name);
  checkCharacters(outPath, text);
  checkSyntax(outPath, text);
  // Compared with line endings normalised. Git is configured to check these
  // files out with CRLF on Windows and the generator emits LF, so a literal
  // comparison would call every file stale on a fresh Windows clone while
  // passing in CI. The board does not care either way: the upload protocol
  // strips \r a line at a time.
  const onDisk = existsSync(outPath) ? readFileSync(outPath, "utf8") : null;
  const existing = onDisk === null ? null : onDisk.split("\r\n").join("\n");
  if (check) {
    if (existing !== text) {
      console.error(`stale: ${outPath} does not match app/src/${name}.jsx`);
      stale++;
    }
    continue;
  }
  if (existing === text) {
    console.log(`unchanged  ${outPath}`);
  } else {
    writeFileSync(outPath, text);
    const lines = text.split("\n").length;
    console.log(`built      ${outPath}  (${text.length} chars, ${lines} lines)`);
  }
}

if (stale) {
  console.error(`\n${stale} output(s) out of date — run: node tools/build-app.mjs`);
  process.exit(1);
}
if (check) console.log(`ok — ${targets.length} built app(s) match their sources`);
