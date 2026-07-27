// jsx.mjs — turn JSX into h() calls, with no dependencies.
//
// The board runs plain ES2023 that QuickJS can parse, so JSX has to be gone
// before a script is pushed. Babel and esbuild both do this well and both cost
// an npm tree; CI here runs `node --check` and one script, and that is worth
// keeping. This is the transform, and only the transform:
//
//   <label text="hi" />              -> h("label", {text: "hi"})
//   <Row a={1} {...rest}>x</Row>     -> h(Row, Object.assign({a: 1}, rest), "x")
//   <>{a}{b}</>                      -> h(Fragment, {}, a, b)
//
// Lowercase names become strings (the runtime reads them as `lv` maker names);
// anything capitalised or dotted stays an expression, so it resolves to the
// component of that name in scope. That is JSX's own rule.
//
// Line numbers are preserved: every newline consumed inside a JSX region is
// re-emitted after the expression it belonged to, so a stack trace from the
// board still points at the right line of the source .jsx.
//
// What it does not do: entity escapes (&amp;), namespaced attributes
// (xlink:href), or TypeScript. A regular expression literal containing an
// unbalanced brace or quote inside a JSX expression container will also confuse
// the scanner — write it outside the JSX if you ever need one.

const ID_START = /[A-Za-z_$]/;
const ID_CHAR = /[A-Za-z0-9_$]/;

// After one of these, a `<` starts JSX and a `/` starts a regex. After an
// identifier, number, string, `)` or `]`, both are operators instead.
const EXPR_AFTER = new Set([
  "", "(", "[", "{", ",", ";", ":", "=", "+", "-", "*", "/", "%", "!", "&",
  "|", "^", "~", "?", "<", ">", "\n",
]);
const EXPR_KEYWORDS = new Set([
  "return", "case", "in", "of", "typeof", "instanceof", "new", "delete", "void",
  "do", "else", "yield", "await", "throw",
]);

export function transformJsx(src, file = "<input>") {
  let i = 0;
  const n = src.length;
  let lastTok = "";    // last significant character
  let lastWord = "";   // last identifier, for the keyword cases

  const fail = msg => {
    const line = src.slice(0, i).split("\n").length;
    throw new Error(`${file}:${line}: ${msg}`);
  };

  const exprPos = () => EXPR_AFTER.has(lastTok) || EXPR_KEYWORDS.has(lastWord);

  const note = ch => {
    if (ch === " " || ch === "\t" || ch === "\r") return;
    if (ID_CHAR.test(ch)) {
      lastWord = ID_CHAR.test(lastTok) ? lastWord + ch : ch;
      lastTok = ch;
    } else {
      lastWord = "";
      lastTok = ch;
    }
  };

  // ------------------------------------------------------------ literals

  const readString = () => {
    const q = src[i];
    let out = src[i++];
    while (i < n) {
      const c = src[i];
      out += c;
      i++;
      if (c === "\\") { out += src[i]; i++; continue; }
      if (c === q) break;
    }
    lastTok = "x"; lastWord = "";
    return out;
  };

  const readTemplate = () => {
    let out = src[i++];   // `
    while (i < n) {
      const c = src[i];
      if (c === "\\") { out += c + src[i + 1]; i += 2; continue; }
      if (c === "`") { out += c; i++; break; }
      if (c === "$" && src[i + 1] === "{") {
        out += "${";
        i += 2;
        const save = [lastTok, lastWord];
        lastTok = "{"; lastWord = "";
        out += readCode("}");
        [lastTok, lastWord] = save;
        if (src[i] !== "}") fail("unterminated ${ } in a template literal");
        out += "}";
        i++;
        continue;
      }
      out += c;
      i++;
    }
    lastTok = "x"; lastWord = "";
    return out;
  };

  const readRegex = () => {
    let out = src[i++];   // /
    let inClass = false;
    while (i < n) {
      const c = src[i];
      out += c;
      i++;
      if (c === "\\") { out += src[i]; i++; continue; }
      if (c === "[") inClass = true;
      else if (c === "]") inClass = false;
      else if (c === "/" && !inClass) break;
    }
    while (i < n && ID_CHAR.test(src[i])) out += src[i++];
    lastTok = "x"; lastWord = "";
    return out;
  };

  // ------------------------------------------------------------ code

  // Copies source, transforming any JSX it meets, until `close` is reached at
  // nesting depth zero (or EOF when close is null).
  const readCode = close => {
    let out = "";
    let depth = 0;
    while (i < n) {
      const c = src[i];
      if (close && depth === 0 && c === close) break;

      if (c === '"' || c === "'") { out += readString(); continue; }
      if (c === "`") { out += readTemplate(); continue; }
      if (c === "/" && src[i + 1] === "/") {
        while (i < n && src[i] !== "\n") out += src[i++];
        continue;
      }
      if (c === "/" && src[i + 1] === "*") {
        out += "/*"; i += 2;
        while (i < n && !(src[i] === "*" && src[i + 1] === "/")) out += src[i++];
        out += "*/"; i += 2;
        continue;
      }
      if (c === "/" && exprPos()) { out += readRegex(); continue; }
      if (c === "<" && exprPos() && ID_START.test(src[i + 1] || "") ) {
        out += readElement();
        lastTok = ")"; lastWord = "";
        continue;
      }
      if (c === "<" && exprPos() && src[i + 1] === ">") {
        out += readElement();
        lastTok = ")"; lastWord = "";
        continue;
      }

      if (c === "(" || c === "[" || c === "{") depth++;
      else if (c === ")" || c === "]" || c === "}") depth--;
      note(c);
      out += c;
      i++;
    }
    return out;
  };

  // ------------------------------------------------------------ JSX

  const skipSpace = () => { while (i < n && /\s/.test(src[i])) i++; };

  const readName = () => {
    let name = "";
    while (i < n && (ID_CHAR.test(src[i]) || src[i] === "." || src[i] === "-")) name += src[i++];
    return name;
  };

  const newlinesIn = (from, to) => {
    let k = 0;
    for (let p = from; p < to; p++) if (src[p] === "\n") k++;
    return k;
  };

  // <a>...</a> or <a/> or <>...</>. Returns the h(...) call.
  //
  // Newlines are put back where the source had them — before the attribute or
  // child that followed them — rather than all at the end. That keeps the
  // output's line numbers matching the .jsx line for line, and it keeps
  // generated lines short, which matters on the way to the board: the host
  // truncates a serial line past 4 KB and push.ps1 paces per line.
  const readElement = () => {
    const start = i;
    i++;   // <
    const name = src[i] === ">" ? "" : readName();
    if (!name && src[i] !== ">") fail("expected an element name after <");

    const attrs = [];
    let spread = false;
    let selfClosing = false;
    let seen = start;   // everything up to here has had its newlines accounted for

    // Newlines since the last thing that accounted for its own. Anything whose
    // emitted code already carries the newlines it consumed (a nested element,
    // an expression copied through verbatim) calls mark() afterwards, or they
    // would be counted a second time in the remainder at the end.
    const since = () => {
      const nl = newlinesIn(seen, i);
      seen = i;
      return nl;
    };
    const mark = () => { seen = i; };

    if (name) {
      for (;;) {
        skipSpace();
        if (i >= n) fail(`unterminated <${name}>`);
        if (src[i] === "/" && src[i + 1] === ">") { selfClosing = true; i += 2; break; }
        if (src[i] === ">") { i++; break; }
        const nl = since();

        if (src[i] === "{") {
          i++;
          skipSpace();
          if (src.slice(i, i + 3) !== "...") fail("only {...spread} is allowed in an attribute position");
          i += 3;
          const expr = readCode("}");
          if (src[i] !== "}") fail("unterminated {...spread}");
          i++;
          attrs.push({ spread: true, expr: tidy(expr), nl });
          mark();
          spread = true;
          continue;
        }

        const attr = readName();
        if (!attr) fail(`unexpected "${src[i]}" in <${name}>`);
        skipSpace();
        if (src[i] !== "=") { attrs.push({ name: attr, expr: "true", nl }); continue; }
        i++;
        skipSpace();
        if (src[i] === '"' || src[i] === "'") {
          // No mark(): a newline inside a quoted attribute becomes \n in the
          // emitted string, so the line it occupied still has to be given back.
          attrs.push({ name: attr, expr: JSON.stringify(readString().slice(1, -1)), nl });
        } else if (src[i] === "{") {
          i++;
          const expr = readCode("}");
          if (src[i] !== "}") fail(`unterminated {} in attribute ${attr}`);
          i++;
          if (!expr.trim()) fail(`attribute ${attr} has an empty {}`);
          attrs.push({ name: attr, expr: tidy(expr), nl });
          mark();
        } else if (src[i] === "<") {
          attrs.push({ name: attr, expr: readElement(), nl });
          mark();
        } else {
          fail(`attribute ${attr} needs a "string" or a {value}`);
        }
      }
    } else {
      i++;   // the > of <>
    }
    // `seen` deliberately stays where the last attribute left it. A newline
    // between the final attribute and the closing `>` still has to come back,
    // and it does — as the first child's leading newlines, or in the remainder
    // below for a self-closing tag.

    const kids = selfClosing ? [] : readChildren(name, since, mark);

    const type = !name ? "Fragment"
               : /^[a-z]/.test(name) && !name.includes(".") ? JSON.stringify(name)
               : name;

    let props;
    if (!attrs.length) {
      props = "{}";
    } else if (!spread) {
      props = "{" + attrs.map((a, k) => lead(a.nl, k) + `${propKey(a.name)}: ${a.expr}`).join(",") + "}";
    } else {
      // Object.assign rather than object spread: the merge order is the same,
      // and one call is cheaper than building an intermediate object per
      // spread on an engine where every allocation is PSRAM.
      const parts = [];
      let plain = [];
      let plainNl = 0;
      const closePlain = () => {
        if (!plain.length) return;
        parts.push({ nl: plainNl, text: "{" + plain.join(", ") + "}" });
        plain = [];
        plainNl = 0;
      };
      for (const a of attrs) {
        if (a.spread) {
          closePlain();
          parts.push({ nl: a.nl, text: a.expr });
        } else {
          if (!plain.length) plainNl = a.nl;
          plain.push(`${propKey(a.name)}: ${a.expr}`);
        }
      }
      closePlain();
      props = "Object.assign({}" + parts.map(p => "," + lead(p.nl, 1) + p.text).join("") + ")";
    }

    let out = `h(${type}, ${props}`;
    for (const kid of kids) out += "," + lead(kid.nl, 1) + kid.code;
    out += ")";
    // Anything left (newlines after the last child, or inside the closing tag)
    // goes on the end so the running total still matches the source.
    return out + "\n".repeat(newlinesIn(seen, i));
  };

  // A separator that carries the source's newlines. Falls back to a space so
  // the output stays readable when everything was on one line.
  const lead = (nl, index) => (nl ? "\n".repeat(nl) : index === 0 ? "" : " ");

  // Trims spaces and tabs but never newlines: mark() has already declared
  // this text responsible for the lines it spans, so dropping one here would
  // shift everything after it.
  const tidy = s2 => s2.replace(/^[ 	]+/, "").replace(/[ 	]+$/, "");

  const propKey = name => (/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name) ? name : JSON.stringify(name));

  const readChildren = (name, since, mark) => {
    const kids = [];
    let text = "";

    const flushText = () => {
      const t = squashJsxText(text);
      text = "";
      // Whitespace-only text is not a child, but its newlines still belong to
      // whatever comes next, which `since()` picks up.
      if (t) kids.push({ code: JSON.stringify(t), nl: 0 });
    };

    for (;;) {
      if (i >= n) fail(`unterminated <${name || ""}>`);

      if (src[i] === "<" && src[i + 1] === "/") {
        flushText();
        i += 2;
        const close = src[i] === ">" ? "" : readName();
        skipSpace();
        if (src[i] !== ">") fail(`unterminated closing tag for <${name}>`);
        i++;
        if (close !== name) fail(`<${name}> is closed by </${close}>`);
        return kids;
      }
      if (src[i] === "<") {
        flushText();
        const nl = since();
        kids.push({ code: readElement(), nl });
        mark();
        continue;
      }
      if (src[i] === "{") {
        flushText();
        const nl = since();
        i++;
        const expr = readCode("}");
        if (src[i] !== "}") fail("unterminated {} in children");
        i++;
        // {/* a comment */} is a comment, not a child. Its lines still have to
        // come back, so mark() is deliberately not called for one.
        if (expr.trim() && !/^\s*\/\*[\s\S]*\*\/\s*$/.test(expr)) {
          kids.push({ code: tidy(expr), nl });
          mark();
        }
        continue;
      }
      text += src[i++];
    }
  };

  return readCode(null);
}

// JSX text rules: a line that is only whitespace disappears, leading and
// trailing indentation goes, and what is left joins with single spaces. So an
// element indented over three lines is one string, not one with the source's
// newlines baked into it.
function squashJsxText(text) {
  const lines = text.split("\n");
  const kept = [];
  for (let k = 0; k < lines.length; k++) {
    let line = lines[k];
    if (k > 0) line = line.replace(/^\s+/, "");
    if (k < lines.length - 1) line = line.replace(/\s+$/, "");
    if (line) kept.push(line);
  }
  return kept.join(" ");
}
