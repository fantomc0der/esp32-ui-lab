// engine-probe.js: exercises QuickJS-ng itself rather than the bindings. Deploy in
// place of app.js (.\push.ps1 app\engine-probe.js -Dest /app.js) and read the
// serial log; ends with an ENGINEPROBE line giving the count.
let pass = 0, fail = 0;
function check(name, fn) {
  try {
    const r = fn();
    if (r === true) { pass++; console.log("PASS " + name); }
    else { fail++; console.log("FAIL " + name + " -> " + JSON.stringify(r)); }
  } catch (e) { fail++; console.log("FAIL " + name + " threw " + e); }
}
const scr = lv.screen();
lv.label(scr, { align: "center", text: "engine probe" });

// The five sites upstream retyped in d8e1cc6.
check("parseInt radix 16", () => parseInt("ff", 16) === 255);
check("parseInt radix 2/8/36", () => parseInt("101", 2) === 5 && parseInt("777", 8) === 511 && parseInt("z", 36) === 35);
check("parseInt radix 0/absent/out of range", () => parseInt("0x1f") === 31 && parseInt("10", 0) === 10 && Number.isNaN(parseInt("10", 37)));
check("error stack carries line numbers (find_line_num)", () => {
  try { (function boom() {
    null.x;
  })(); } catch (e) { return /:\d+(:\d+)?\)?/.test(e.stack) ? true : e.stack; }
});
check("Atomics.notify returns a count", () => {
  if (typeof Atomics === "undefined" || typeof SharedArrayBuffer === "undefined") return true;
  return Atomics.notify(new Int32Array(new SharedArrayBuffer(8)), 0, 1) === 0;
});

// Regex engine (libregexp changed heavily).
check("regex named groups", () => "2026-09-19".match(/(?<y>\d{4})-(?<m>\d\d)-(?<d>\d\d)/).groups.m === "09");
check("regex lookbehind", () => "price $42".match(/(?<=\$)\d+/)[0] === "42");
check("regex unicode property", () => /^\p{Lu}+$/u.test("ÄÖÜ") && !/^\p{Lu}+$/u.test("äb"));
check("regex sticky + replaceAll", () => { const r = /a/y; r.lastIndex = 1; return r.test("ba") && "a-a-a".replaceAll("-", "+") === "a+a+a"; });
check("regex matchAll + split", () => [..."a1b22c333".matchAll(/\d+/g)].map(m => m[0]).join(",") === "1,22,333" && "a, b,c".split(/\s*,\s*/).length === 3);
check("regex v flag set ops (if supported)", () => { try { return new RegExp("[\\p{L}--[a-z]]", "v").test("A"); } catch (e) { return true; } });

// Unicode tables.
check("case mapping", () => "ß".toUpperCase() === "SS" && "İ".toLowerCase().length === 2 && "ǅ".toLowerCase() === "ǆ");
check("normalize NFC/NFD", () => "é".normalize("NFD").length === 2 && "é".normalize("NFC") === "é");
check("string iteration by code point", () => [..."a😀b"].length === 3 && "😀".codePointAt(0) === 0x1F600);
check("degree sign survives", () => "72°F".length === 4);

// Number formatting (dtoa).
check("float to string", () => String(0.1 + 0.2) === "0.30000000000000004" && String(1e21) === "1e+21" && String(-0) === "0");
check("toFixed/toPrecision/toExponential", () => (123.456).toFixed(2) === "123.46" && (0.000123).toPrecision(2) === "0.00012" && (12345).toExponential(1) === "1.2e+4");
check("parseFloat/Number", () => parseFloat("3.14abc") === 3.14 && Number("1e-7") === 1e-7 && Number.isNaN(Number("x")));
check("toString radix", () => (255).toString(16) === "ff" && (0.5).toString(2) === "0.1");
check("JSON round trip", () => { const o = { a: [1, 2.5, "x", null, true], b: { c: -1e-9 } }; return JSON.stringify(JSON.parse(JSON.stringify(o))) === JSON.stringify(o); });

// General language/runtime.
check("classes, private fields, static blocks", () => { class A { #x = 2; static s; static { A.s = 7; } get x() { return this.#x; } } return new A().x === 2 && A.s === 7; });
check("destructuring, spread, optional chaining", () => { const { a, ...r } = { a: 1, b: 2, c: 3 }; return a === 1 && Object.keys(r).length === 2 && ({})?.q?.w === undefined; });
check("Map/Set/WeakMap", () => new Map([[1, 2]]).get(1) === 2 && new Set([1, 1, 2]).size === 2 && new WeakMap().set({}, 1) instanceof WeakMap);
check("typed arrays + DataView", () => { const b = new ArrayBuffer(8); new DataView(b).setFloat32(0, 1.5); return new DataView(b).getFloat32(0) === 1.5 && new Uint8Array([1, 2, 3]).reduce((s, x) => s + x) === 6; });
check("BigInt", () => (2n ** 64n).toString() === "18446744073709551616");
check("Date", () => { const d = new Date(Date.UTC(2026, 8, 19, 12)); return d.toISOString() === "2026-09-19T12:00:00.000Z" && !Number.isNaN(Date.now()); });
check("generators + iterator helpers", () => { function* g() { yield 1; yield 2; yield 3; } return [...g()].join() === "1,2,3" && (typeof Iterator === "undefined" || g().map(x => x * 2).toArray().join() === "2,4,6"); });
check("Array methods (toSorted, findLast, at, flat)", () => [3, 1, 2].toSorted().join() === "1,2,3" && [1, 2, 3].findLast(x => x < 3) === 2 && [1, 2].at(-1) === 2 && [[1], [2, [3]]].flat(2).length === 3);
check("Object.groupBy / structuredClone-free", () => typeof Object.groupBy !== "function" || Object.groupBy([1, 2, 3], x => x % 2 ? "o" : "e").o.length === 2);
check("string pad/trim/localeCompare", () => "5".padStart(3, "0") === "005" && " x ".trim() === "x" && "a".localeCompare("b") < 0);
check("Proxy + Reflect", () => new Proxy({}, { get: (t, k) => k }).hello === "hello" && Reflect.ownKeys({ a: 1 })[0] === "a");
check("Symbol + WeakRef", () => typeof Symbol.iterator === "symbol" && (typeof WeakRef === "undefined" || new WeakRef({}) instanceof WeakRef));
check("big string join (heap sanity, usable_size trap)", () => { const a = []; for (let i = 0; i < 20000; i++) a.push("item" + i); const s = a.join(","); return s.length > 150000; });
check("big object graph", () => { const m = new Map(); for (let i = 0; i < 20000; i++) m.set("k" + i, { i, s: "v" + i }); return m.get("k19999").i === 19999; });

// Async: Promise.all (remainingElementsCount_add, js_promise_all_resolve_element) and friends.
const asyncChecks = [];
function acheck(name, p) {
  asyncChecks.push(p.then(r => { if (r === true) { pass++; console.log("PASS " + name); } else { fail++; console.log("FAIL " + name + " -> " + JSON.stringify(r)); } },
                           e => { fail++; console.log("FAIL " + name + " rejected " + e); }));
}
acheck("Promise.all keeps order", Promise.all([3, Promise.resolve(1), new Promise(r => { const t = lv.timer(30, () => { t.stop(); r(2); }); })]).then(v => v.join() === "3,1,2"));
acheck("Promise.all over 100 items", Promise.all(Array.from({ length: 100 }, (_, i) => Promise.resolve(i))).then(v => v.length === 100 && v[99] === 99));
acheck("Promise.all empty", Promise.all([]).then(v => v.length === 0));
acheck("Promise.all rejects on first failure", Promise.all([Promise.resolve(1), Promise.reject(new Error("no"))]).then(() => false, e => e.message === "no"));
acheck("Promise.allSettled", Promise.allSettled([Promise.resolve(1), Promise.reject(2)]).then(v => v[0].status === "fulfilled" && v[1].status === "rejected"));
acheck("Promise.any", Promise.any([Promise.reject(1), Promise.resolve(2)]).then(v => v === 2));
acheck("Promise.any all rejected -> AggregateError", Promise.any([Promise.reject(1)]).then(() => false, e => e instanceof AggregateError));
acheck("Promise.race", Promise.race([new Promise(r => { const t = lv.timer(50, () => { t.stop(); r("slow"); }); }), Promise.resolve("fast")]).then(v => v === "fast"));
acheck("async/await + for await", (async () => { let s = 0; for await (const x of [Promise.resolve(1), 2]) s += x; await null; return s === 3; })());
acheck("Array.fromAsync (if present)", typeof Array.fromAsync === "function" ? Array.fromAsync([Promise.resolve(1), 2]).then(v => v.join() === "1,2") : Promise.resolve(true));

Promise.all(asyncChecks).then(() => {
  // Last, because a failure here could take the board down: the stack limit must
  // turn runaway recursion into a catchable RangeError, not a task-stack overflow.
  let depth = 0;
  check("runaway recursion throws RangeError", () => {
    function f() { depth++; return f() + 1; }
    try { f(); return "no throw"; } catch (e) { return e instanceof RangeError ? true : String(e); }
  });
  console.log("recursion depth reached: " + depth);
  check("board still healthy after recursion", () => sys.uptime() > 0 && JSON.stringify({ a: 1 }) === '{"a":1}');
  console.log("heap " + JSON.stringify(sys.heap()));
  console.log(`ENGINEPROBE ${pass} passed, ${fail} failed`);
});
