// widget-methods.mjs — which widget methods a script may call on which widget.
//
// All 17 widget methods sit on one shared prototype, so `.push()` exists on
// every handle and only refuses at runtime: `lv.label(p, {}).push(3)` parses,
// pushes, and then throws on the panel. The pairing that would have caught it
// is already written down in C, twice over, so it is derived here rather than
// restated:
//
//   kMakers[] + js_lv_make's switch   tag -> the LVGL class the maker builds
//   a throwing lv_obj_check_type      method -> the classes it accepts
//
// The JS side is deliberately shallow. A bare identifier is followed only when
// the file leaves no doubt about it: every declaration of that name is a direct
// `lv.<tag>(...)` call, they all agree on the tag, and nothing else in the file
// can rebind it. A widget from `.add()`, a function parameter, a property, a
// reassigned variable — all stay unchecked. The value of this check is that a
// red result always means something, and a guess would cost exactly that.

// ---------------------------------------------------------------- tokenizer

// Enough of a JS tokenizer to tell code from the things that merely look like
// it. A regex holding a `.push(`, a `//` line about `.addTab()`, a template
// literal — a plain regex pass reports all three, and a report that has to be
// double-checked by hand is not worth having.

const ID_START = /[A-Za-z_$]/;
const ID_CHAR = /[A-Za-z0-9_$]/;

// After one of these a `/` opens a regex; after a value, `)`, `]` or `}` it is
// division. `}` is genuinely ambiguous (a block ends, an object literal ends);
// division is the reading that misfires less often, and it is the one
// tools/jsx.mjs already takes.
const EXPR_KEYWORDS = new Set([
  "return", "case", "in", "of", "typeof", "instanceof", "new", "delete", "void",
  "do", "else", "yield", "await", "throw",
]);

const PUNCT = [
  ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "??=",
  "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "**",
  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>",
];

// A line marked this way is skipped. app/selftest.js is the reason it exists:
// its whole point in places is to call a method on a widget that does not have
// it and prove the C guard throws.
const SKIP = /\/\/.*\bcheck-js-api:\s*wrong kind on purpose\b/;

// Returns { tokens, skipLines }. A token is { v, kind, line, dotted }, where
// `kind` is "name", "punc" or "value" (a string, number, template or regex —
// anything that ends an expression) and `dotted` marks an identifier reached
// through `.` or `?.`, i.e. a property rather than a variable.
export function tokenize(src) {
  const tokens = [];
  const skipLines = new Set();
  const n = src.length;
  let i = 0;
  let line = 1;

  const last = () => tokens[tokens.length - 1];

  const push = (v, kind) => {
    const prev = last();
    const dotted = kind === "name" && !!prev &&
                   prev.kind === "punc" && (prev.v === "." || prev.v === "?.");
    tokens.push({ v, kind, line, dotted });
  };

  const regexOpens = () => {
    const p = last();
    if (!p) return true;
    if (p.kind === "name") return EXPR_KEYWORDS.has(p.v);
    if (p.kind === "value") return false;
    return p.v !== ")" && p.v !== "]" && p.v !== "}";
  };

  const advance = to => {
    for (let p = i; p < to; p++) if (src[p] === "\n") line++;
    i = to;
  };

  const readString = () => {
    const q = src[i++];
    while (i < n) {
      const c = src[i];
      if (c === "\\") { advance(i + 2); continue; }
      i++;
      if (c === "\n") line++;
      if (c === q) break;
    }
    push("", "value");
  };

  const readRegex = () => {
    i++;
    let inClass = false;
    while (i < n) {
      const c = src[i];
      if (c === "\\") { advance(i + 2); continue; }
      i++;
      if (c === "[") inClass = true;
      else if (c === "]") inClass = false;
      else if (c === "/" && !inClass) break;
      else if (c === "\n") { line++; break; }   // unterminated; do not run away
    }
    while (i < n && ID_CHAR.test(src[i])) i++;
    push("", "value");
  };

  // A template's substitutions are code, so they are tokenized in place. The
  // template itself contributes one value token at the end, which is what the
  // regex/division rule needs to see afterwards.
  const readTemplate = () => {
    i++;   // `
    for (;;) {
      if (i >= n) break;
      const c = src[i];
      if (c === "\\") { advance(i + 2); continue; }
      if (c === "`") { i++; break; }
      if (c === "$" && src[i + 1] === "{") {
        i += 2;
        run(true);
        if (src[i] === "}") i++;
        continue;
      }
      if (c === "\n") line++;
      i++;
    }
    push("", "value");
  };

  // Reads tokens until EOF, or — when `sub` — until the `}` that closes the
  // template substitution we are inside, which is left for the caller.
  function run(sub) {
    let depth = 0;
    while (i < n) {
      const c = src[i];

      if (c === "\n") { line++; i++; continue; }
      if (c === " " || c === "\t" || c === "\r") { i++; continue; }

      if (c === "/" && src[i + 1] === "/") {
        const end = src.indexOf("\n", i);
        const text = src.slice(i, end === -1 ? n : end);
        if (SKIP.test(text)) skipLines.add(line);
        advance(end === -1 ? n : end);
        continue;
      }
      if (c === "/" && src[i + 1] === "*") {
        const end = src.indexOf("*/", i + 2);
        advance(end === -1 ? n : end + 2);
        continue;
      }

      if (c === '"' || c === "'") { readString(); continue; }
      if (c === "`") { readTemplate(); continue; }
      if (c === "/" && regexOpens()) { readRegex(); continue; }

      if (ID_START.test(c)) {
        let j = i;
        while (j < n && ID_CHAR.test(src[j])) j++;
        push(src.slice(i, j), "name");
        i = j;
        continue;
      }
      if (/[0-9]/.test(c)) {
        let j = i;
        while (j < n && /[0-9a-fA-FxXoObBeE._n]/.test(src[j])) j++;
        push(src.slice(i, j), "value");
        i = j;
        continue;
      }

      if (sub && c === "}" && depth === 0) return;
      if (c === "{") depth++;
      else if (c === "}") depth--;

      const op = PUNCT.find(p => src.startsWith(p, i));
      push(op ?? c, "punc");
      i += op ? op.length : 1;
    }
  }

  run(false);
  return { tokens, skipLines };
}

// ---------------------------------------------------------------- from the C

// Reads the two tables out of bindings_lv.cpp. Throws rather than returning an
// empty model: an empty one would let every script pass, which is the one
// failure mode a check like this must not have.
export function deriveModel(cSource) {
  // {"list", W_LIST} -> tag to enumerator, then
  // case W_LIST: obj = lv_list_create(...) -> enumerator to LVGL class.
  const kindOfTag = new Map();
  const makersBlock = cSource.match(/kMakers\[\]\s*=\s*\{([\s\S]*?)\};/);
  if (makersBlock) {
    for (const m of makersBlock[1].matchAll(/\{\s*"(\w+)"\s*,\s*(\w+)\s*\}/g)) {
      kindOfTag.set(m[1], m[2]);
    }
  }
  const classOfKind = new Map();
  for (const m of cSource.matchAll(/case\s+(\w+):\s*obj\s*=\s*lv_(\w+)_create\s*\(/g)) {
    classOfKind.set(m[1], m[2]);
  }

  const classOfTag = new Map();
  for (const [tag, kind] of kindOfTag) {
    const cls = classOfKind.get(kind);
    if (cls) classOfTag.set(tag, cls);
  }

  // The prototype's method names, and the C function behind each one.
  const fnOfMethod = new Map();
  const re = /JS_SetPropertyStr\(\s*ctx,\s*wproto,\s*"(\w+)",\s*JS_NewCFunction\(\s*ctx,\s*(\w+)/g;
  for (const m of cSource.matchAll(re)) fnOfMethod.set(m[1], m[2]);

  // Each restriction is one early return: `if (!lv_obj_check_type(obj,
  // &lv_<class>_class)) return JS_ThrowTypeError(...)`. Attributing a guard to
  // the nearest function header above it avoids having to match C braces, and
  // the other lv_obj_check_type calls in this file — the ones that pick a
  // behaviour rather than refuse one — do not match this shape at all.
  const starts = [];
  for (const m of cSource.matchAll(/JSValue\s+(\w+)\s*\(\s*JSContext/g)) {
    starts.push([m.index, m[1]]);
  }
  const guard = /if\s*\(!lv_obj_check_type\(\s*\w+,\s*&lv_(\w+)_class\)\)\s*return\s+JS_ThrowTypeError/g;
  const classesOfFn = new Map();
  for (const m of cSource.matchAll(guard)) {
    let owner = null;
    for (const [at, name] of starts) {
      if (at < m.index) owner = name; else break;
    }
    if (owner) (classesOfFn.get(owner) ?? classesOfFn.set(owner, new Set()).get(owner)).add(m[1]);
  }

  const classesOfMethod = new Map();
  for (const [method, fn] of fnOfMethod) {
    const classes = classesOfFn.get(fn);
    if (classes) classesOfMethod.set(method, classes);
  }

  if (classOfTag.size === 0)
    throw new Error("no widget makers found — kMakers[] or js_lv_make moved");
  if (fnOfMethod.size === 0)
    throw new Error("no widget methods found — the wproto prototype moved");
  if (classesOfMethod.size === 0)
    throw new Error("no per-kind method guards found — lv_obj_check_type moved");

  // Reporting reads better in the vocabulary a script is written in, so a
  // guard's LVGL class is named back as the maker that builds it.
  const tagOfClass = new Map();
  for (const [tag, cls] of classOfTag) tagOfClass.set(cls, tag);

  return { classOfTag, classesOfMethod, tagOfClass };
}

// ---------------------------------------------------------------- the scripts

// Methods that hand back the same widget, so a chain can be followed through
// them. Everything else ends the chain: `.add()` and `.addTab()` return a
// different widget, `.bounds()` a plain object, `.value()` either the widget or
// its value depending on the arguments.
const CHAINS = new Set(["set", "on"]);

const CONTROL = new Set(["if", "while", "for", "switch"]);

const isPunc = (t, v) => !!t && t.kind === "punc" && t.v === v;
const isName = t => !!t && t.kind === "name";
const isAccess = t => isPunc(t, ".") || isPunc(t, "?.");

// Index of the token closing the group that opens at `open`.
function matchGroup(tokens, open) {
  const pairs = { "(": ")", "[": "]", "{": "}" };
  const close = pairs[tokens[open].v];
  let depth = 0;
  for (let k = open; k < tokens.length; k++) {
    const t = tokens[k];
    if (t.kind !== "punc") continue;
    if (pairs[t.v]) depth++;
    else if (t.v === ")" || t.v === "]" || t.v === "}") {
      depth--;
      if (depth === 0) return t.v === close ? k : -1;
    }
  }
  return -1;
}

// `lv.<tag>(` at `k`, on the real `lv` global rather than some object's
// property. Returns the tag, or null.
function makerAt(tokens, k, classOfTag) {
  const t = tokens[k];
  if (!isName(t) || t.v !== "lv" || t.dotted) return null;
  if (!isPunc(tokens[k + 1], ".")) return null;
  const tag = tokens[k + 2];
  if (!isName(tag) || !classOfTag.has(tag.v)) return null;
  if (!isPunc(tokens[k + 3], "(")) return null;
  return tag.v;
}

// Every bare identifier the file rebinds in a way this scan cannot follow.
// Being generous here only costs coverage; missing one would cost a false
// report, which is the more expensive mistake.
function shadowedNames(tokens) {
  const out = new Set();
  const claimGroup = open => {
    const close = matchGroup(tokens, open);
    if (close === -1) return;
    for (let k = open + 1; k < close; k++) {
      if (isName(tokens[k]) && !tokens[k].dotted) out.add(tokens[k].v);
    }
  };

  for (let k = 0; k < tokens.length; k++) {
    const t = tokens[k];

    if (isName(t) && !t.dotted) {
      // function f(...) / class C / catch (e) all introduce a name.
      if ((t.v === "function" || t.v === "class") && isName(tokens[k + 1])) out.add(tokens[k + 1].v);
      // x => ...
      if (isPunc(tokens[k + 1], "=>")) out.add(t.v);
      // x = ..., x += ..., x++ — anything that can replace what x holds.
      const next = tokens[k + 1];
      const declared = isName(tokens[k - 1]) && /^(const|let|var)$/.test(tokens[k - 1].v);
      if (!declared && next && next.kind === "punc" &&
          (next.v === "=" || next.v === "++" || next.v === "--" || /^[^=!<>]+=$/.test(next.v))) {
        out.add(t.v);
      }
      // const {a, b} = ... / const [a] = ... — a binding this scan cannot read.
      if (/^(const|let|var)$/.test(t.v) &&
          (isPunc(tokens[k + 1], "{") || isPunc(tokens[k + 1], "["))) {
        claimGroup(k + 1);
      }
    }

    if (isPunc(t, "(")) {
      const close = matchGroup(tokens, k);
      if (close === -1) continue;
      const before = tokens[k - 1];
      const after = tokens[close + 1];
      const params =
        isPunc(after, "=>") ||                                   // (a, b) => ...
        (isName(before) && (before.v === "function" || before.v === "catch")) ||
        // f(a, b) { ... } — a method shorthand. `if (x) { ... }` has the same
        // shape, so the control keywords are excluded rather than checked.
        (isName(before) && !before.dotted && !CONTROL.has(before.v) && isPunc(after, "{"));
      if (params) claimGroup(k);
    }
  }
  return out;
}

// name -> tag, for the identifiers whose widget kind the file settles beyond
// doubt. A name declared twice keeps its kind only if both agree.
function widgetLocals(tokens, classOfTag) {
  const shadowed = shadowedNames(tokens);
  const kinds = new Map();
  const conflicted = new Set();

  for (let k = 0; k < tokens.length; k++) {
    const t = tokens[k];
    if (!isName(t) || t.dotted || !/^(const|let|var)$/.test(t.v)) continue;
    const name = tokens[k + 1];
    if (!isName(name)) continue;
    // `for (const k of …)` and a bare `let x;` bind the name too, so they have
    // to disqualify it rather than simply not being a maker declaration.
    if (!isPunc(tokens[k + 2], "=")) { shadowed.add(name.v); continue; }

    const tag = makerAt(tokens, k + 3, classOfTag);
    if (!tag) { shadowed.add(name.v); continue; }
    const seen = kinds.get(name.v);
    if (seen !== undefined && seen !== tag) conflicted.add(name.v);
    kinds.set(name.v, tag);
  }

  for (const name of [...shadowed, ...conflicted]) kinds.delete(name);
  return kinds;
}

// Reports { line, receiver, method, tag, allowed } for every call this scan is
// certain about and the firmware would refuse.
export function findWrongKindCalls(src, model) {
  const { classOfTag, classesOfMethod, tagOfClass } = model;
  const { tokens, skipLines } = tokenize(src);
  const locals = widgetLocals(tokens, classOfTag);
  const problems = [];

  // Walks `.method(...)` links from the token after a call's `)`, as long as
  // each one is known to hand the same widget back.
  const followChain = (from, tag, receiver) => {
    let k = from;
    for (;;) {
      if (!isAccess(tokens[k]) || !isName(tokens[k + 1]) || !isPunc(tokens[k + 2], "(")) return;
      const method = tokens[k + 1];
      const allowed = classesOfMethod.get(method.v);
      if (allowed && !allowed.has(classOfTag.get(tag)) && !skipLines.has(method.line)) {
        problems.push({
          line: method.line,
          receiver,
          method: method.v,
          tag,
          allowed: [...allowed].map(cls => tagOfClass.get(cls) ?? cls),
        });
      }
      if (!CHAINS.has(method.v)) return;
      const close = matchGroup(tokens, k + 2);
      if (close === -1) return;
      k = close + 1;
    }
  };

  for (let k = 0; k < tokens.length; k++) {
    // lv.chart(p, {}).push(3) — the kind is right there in the expression.
    const tag = makerAt(tokens, k, classOfTag);
    if (tag) {
      const close = matchGroup(tokens, k + 3);
      if (close !== -1) followChain(close + 1, tag, `lv.${tag}()`);
      continue;
    }
    // chart.push(3), where the file said what `chart` is.
    const t = tokens[k];
    if (isName(t) && !t.dotted && locals.has(t.v) && isAccess(tokens[k + 1])) {
      followChain(k + 1, locals.get(t.v), t.v);
    }
  }

  return problems;
}
