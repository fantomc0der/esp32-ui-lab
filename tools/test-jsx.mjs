// test-jsx.mjs — the JSX transform's tests.
//
//   node tools/test-jsx.mjs
//
// Two things are being checked, and the second matters more than the first.
// One: the shapes that appear in an app come out as the right h() call. Two:
// code that merely *looks* like JSX is left alone — `a < b`, a regex holding a
// `<`, a `<` inside a string or a comment. A transform that gets that wrong
// corrupts a file that would otherwise have worked, which is worse than
// refusing to build it.

import { transformJsx } from "./jsx.mjs";

let passed = 0;
const failures = [];

// Shape: what h() call came out, ignoring where the transform put its
// newlines. Those are the source's own, put back so line numbers survive, and
// they are checked separately below.
function is(input, expected, what) {
  const actual = transformJsx(input).trim().replace(/\s+/g, " ").replace(/\{ /g, "{").replace(/ \}/g, "}");
  if (actual === expected) { passed++; return; }
  failures.push([what || input, expected, actual]);
}

// Byte for byte, for the inputs that must come through untouched.
function same(input, what) {
  const actual = transformJsx(input);
  if (actual === input) { passed++; return; }
  failures.push([what, JSON.stringify(input), JSON.stringify(actual)]);
}

function throws(input, what) {
  try {
    transformJsx(input);
    failures.push([what, "an error", "no error"]);
  } catch {
    passed++;
  }
}

// ---------------------------------------------------------------- elements

is(`<label />`, `h("label", {})`, "self-closing, no props");
is(`<label text="hi" />`, `h("label", {text: "hi"})`, "string prop");
is(`<label w={100} />`, `h("label", {w: 100})`, "expression prop");
is(`<label busy />`, `h("label", {busy: true})`, "bare prop is true");
is(`<Row a={1} />`, `h(Row, {a: 1})`, "a capitalised name is an identifier");
is(`<ui.Row a={1} />`, `h(ui.Row, {a: 1})`, "a dotted name is an expression");
is(`<label>hi</label>`, `h("label", {}, "hi")`, "text child");
is(`<obj><label /></obj>`, `h("obj", {}, h("label", {}))`, "element child");
is(`<obj>{value}</obj>`, `h("obj", {}, value)`, "expression child");
is(`<>{a}{b}</>`, `h(Fragment, {}, a, b)`, "fragment");
is(`<obj>{/* nothing */}</obj>`, `h("obj", {})`, "a comment child is not a child");

is(`<obj a={1} {...rest} b={2} />`,
   `h("obj", Object.assign({}, {a: 1}, rest, {b: 2}))`,
   "spread merges in order");
is(`<obj {...rest} />`, `h("obj", Object.assign({}, rest))`, "spread alone");

is(`<button onClick={() => go(n + 1)}>Next</button>`,
   `h("button", {onClick: () => go(n + 1)}, "Next")`,
   "an arrow function prop");

is(`<obj>{items.map(i => <label key={i} text={i} />)}</obj>`,
   `h("obj", {}, items.map(i => h("label", {key: i, text: i})))`,
   "JSX nested inside an expression child");

is(`<obj>{on ? <label text="y" /> : <label text="n" />}</obj>`,
   `h("obj", {}, on ? h("label", {text: "y"}) : h("label", {text: "n"}))`,
   "JSX in both arms of a conditional");

is(`<obj>{on && <label />}</obj>`, `h("obj", {}, on && h("label", {}))`, "&& guard");

// Comments between attributes. Real apps annotate a prop, and the transform
// used to reject the file — found by porting apps/vitals.js, not by writing
// tests. Note the asymmetry, which is JSX's own: between attributes a comment
// is a comment, but in the children position it is text, so {/* … */} is the
// form that works down there.
is(`<obj\n  a={1}\n  // why b\n  b={2}\n/>`, `h("obj", {a: 1, b: 2})`, "// comment between attributes");
is(`<obj a={1} /* why b */ b={2} />`, `h("obj", {a: 1, b: 2})`, "/* */ comment between attributes");
is(`<obj\n  // trailing note\n/>`, `h("obj", {})`, "a comment as the only thing in the tag");
is(`<obj>// not a comment</obj>`, `h("obj", {}, "// not a comment")`, "a slash pair in children is text");

// ---------------------------------------------------------------- text rules

is(`<obj>
      hello
    </obj>`, `h("obj", {}, "hello")`, "indentation around text is dropped");

is(`<label>a {b} c</label>`, `h("label", {}, "a ", b, " c")`,
   "text either side of an expression keeps its spaces");

is(`<obj>
      <label />
      <label />
    </obj>`, `h("obj", {}, h("label", {}), h("label", {}))`,
   "whitespace between elements is not a child");

// ---------------------------------------------------------------- not JSX

same(`const c = a < b;`, "less-than after an identifier");
same(`if (x<y && y>z) {}`, "a comparison chain");
same(`const n = count<10 ? 1 : 2;`, "less-than before a number");
same(`const r = /<label>/.test(s);`, "a regex containing a tag");
same(`const s = "<label />";`, "a tag inside a string");
same("const s = `<label ${x} />`;", "a tag inside a template");
same(`// <label />\nconst x = 1;`, "a tag inside a line comment");
same(`/* <label /> */\nconst x = 1;`, "a tag inside a block comment");
same(`const f = a / b / c;`, "division is not a regex");
same(`const v = arr[0] < arr[1];`, "less-than after an index");
same(`const s = "a < b and c > d";`, "comparisons inside a string");

// ---------------------------------------------------------------- line numbers

// Every line of the source has to stay on its own line of the output, or a
// stack trace from the board points at the wrong place in the .jsx.
{
  const src = [
    `const a = 1;`,
    `const el = <obj`,
    `  w={10}`,
    `  h={20}>`,
    `  <label text="x" />`,
    `  {items.map(i => (`,
    `    <label key={i} />`,
    `  ))}`,
    `</obj>;`,
    `const z = 2;`,
  ].join("\n");
  const out = transformJsx(src);
  const lines = out.split("\n");
  const wrong = [];
  const expect = (nth, needle) => {
    if (!(lines[nth] || "").includes(needle)) wrong.push(`line ${nth + 1} should hold ${needle}, got ${JSON.stringify(lines[nth])}`);
  };
  expect(0, "const a = 1;");
  expect(2, "w: 10");
  expect(3, "h: 20");
  expect(4, '"label"');
  expect(5, "items.map");
  expect(9, "const z = 2;");
  if (out.split("\n").length !== src.split("\n").length) {
    wrong.push(`output has ${out.split("\n").length} lines, source has ${src.split("\n").length}`);
  }
  if (wrong.length) failures.push(["line numbers survive a multi-line element", "same line for same content", wrong.join("; ")]);
  else passed++;
}

// A self-closing tag whose `/>` sits on its own line: the newline in front of
// it belongs to nobody, and used to be dropped.
{
  const cases = [
    `const a = <Row\n  x={1}\n  y={2}\n/>;\n`,
    `const b = <obj\n  w={1}\n>\n  <label />\n</obj>;\n`,
    `const c = <>\n  <label />\n</>;\n`,
    `const d = <obj>\n  {list.map(i => (\n    <label key={i} />\n  ))}\n</obj>;\n`,
    `const e = <obj a="one" b={two}>text</obj>;\n`,
    // A comment child is dropped, but the line it sat on — and the newline in
    // front of it — still have to come back. Found by porting apps/vitals.js.
    `const f = <obj>\n  {/* why */}\n  <label />\n</obj>;\n`,
    `const g = <obj>\n  {/* only child */}\n</obj>;\n`,
    `const h2 = <obj\n  a={1}\n  // note\n  b={2}\n/>;\n`,
  ];
  for (const src of cases) {
    const out = transformJsx(src);
    const want = src.split("\n").length, got = out.split("\n").length;
    if (want === got) passed++;
    else failures.push([`line count for ${JSON.stringify(src.slice(0, 24))}...`, `${want} lines`, `${got} lines`]);
  }
}

// ---------------------------------------------------------------- errors

throws(`<obj>`, "an unclosed element is an error");
throws(`<obj></label>`, "a mismatched closing tag is an error");
throws(`<obj a= />`, "an attribute with no value is an error");
throws(`<obj a={} />`, "an attribute with an empty {} is an error");

// ---------------------------------------------------------------- report

if (failures.length) {
  for (const [what, expected, actual] of failures) {
    console.error(`FAIL  ${what}`);
    console.error(`      expected: ${expected}`);
    console.error(`      actual:   ${actual}`);
  }
  console.error(`\n${failures.length} failed, ${passed} passed`);
  process.exit(1);
}
console.log(`ok — ${passed} JSX transform tests passed`);
