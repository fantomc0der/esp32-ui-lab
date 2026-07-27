// ui-selftest.jsx — the component runtime, checked on the panel.
//
// @out app/ui-selftest.js
//
// Built outside app/apps/ on purpose: the launcher lists everything in there,
// and a test is not one of the board's apps. Same reason app/selftest.js sits
// where it does.
//
//   node tools/build-app.mjs ui-selftest
//   .\push.ps1 app\ui-selftest.js -Dest /app.js
//
// tools/test-ui.mjs covers the same ground against a fake `lv` and covers more
// of it, because a mock can count things hardware cannot. What it cannot do is
// prove that the bookkeeping matches LVGL: that lv_obj_move_to_index really
// reorders, that a deleted widget really invalidates its handle, that a render
// deferred into a promise microtask really lands within a frame. That is what
// this is for, and it is why it earns its place next to selftest.js rather
// than duplicating it.
//
// Prints PASS/FAIL per check and ends with a UISELFTEST line, same as
// selftest.js. Steps are separated by a real LVGL timer tick, so each one runs
// after lv_timer_handler() and jsvm_pump() have both been round.

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

const threw = fn => { try { fn(); return false; } catch (e) { return true; } };

// One real frame: the timer fires from inside lv_timer_handler(), so by the
// time this resolves the host has also pumped the job queue that the runtime
// schedules its renders on.
const frame = () => new Promise(done => {
  const t = lv.timer(20, () => { t.stop(); done(); });
});

// ---------------------------------------------------------------- the tree

// A plain object, not useRef(): hooks only work inside a component, and this
// has to be reachable from the checks below.
const box = { current: null };

let setRows, setLabel, setTicking;

const rowRefs = {};
const kept = {};

function Row({ id, text }) {
  return <label key={id} ref={w => { if (w) rowRefs[id] = w; }} text={text} font={14} color="#F0F4F8" />;
}

let effectLog = [];
let intervalTicks = 0;

function Ticker() {
  useEffect(() => {
    effectLog.push("up");
    return () => effectLog.push("down");
  }, []);
  useInterval(() => { intervalTicks++; }, 30);
  return <label text="ticking" hidden />;
}

function Probe() {
  const [rows, setR] = useState(["a", "b", "c"]);
  const [label, setL] = useState("first");
  const [ticking, setT] = useState(true);
  setRows = setR;
  setLabel = setL;
  setTicking = setT;

  return (
    <obj ref={box} w="100%" h="100%" bg="#101418" border={0} radius={0} pad={4} scroll={false}>
      <label ref={w => { if (w) kept.head = w; }} text={label} font={16} color="#7FC4FF" />
      {rows.map(id => <Row key={id} id={id} text={"row " + id} />)}
      {ticking && <Ticker />}
    </obj>
  );
}

render(<Probe />);

// ---------------------------------------------------------------- checks

async function run() {
  check("render() mounted a container", () => box.current !== null);
  check("children are in declaration order", () =>
    kept.head.index() === 0 && rowRefs.a.index() === 1 &&
    rowRefs.b.index() === 2 && rowRefs.c.index() === 3);
  check("a ref points at a live widget", () => typeof kept.head.bounds().w === "number");
  check("an effect ran after mount", () => effectLog.length === 1 && effectLog[0] === "up");

  // A state write must not render immediately: that is the whole reason a
  // click handler is allowed to replace the screen.
  setLabel("second");
  check("a state write does not render on the spot", () => kept.head.index() === 0);

  await frame();
  check("the render landed within a frame", () => effectLog.length === 1);

  // Patched in place: the widget the ref captured before the render is still
  // valid, which it would not be if the runtime had rebuilt the subtree.
  const headBefore = kept.head;
  await frame();
  check("a re-render patches rather than rebuilds", () => {
    kept.head.set({ hidden: false });
    return kept.head === headBefore;
  });

  // Reorder. Every row survives and moves; none is recreated.
  const a = rowRefs.a, b = rowRefs.b, c = rowRefs.c;
  setRows(["c", "a", "b"]);
  await frame();
  check("a keyed reorder moves the same widgets", () =>
    rowRefs.a === a && rowRefs.b === b && rowRefs.c === c);
  check("lv_obj_move_to_index put them in the new order", () =>
    c.index() === 1 && a.index() === 2 && b.index() === 3);
  check("the unkeyed sibling stayed put", () => kept.head.index() === 0);

  // Removal. The handle held for the removed row must go stale, exactly as it
  // does after a .clean() — this is the runtime calling .delete() on it.
  setRows(["c", "b"]);
  await frame();
  check("a removed child's widget is deleted", () => threw(() => a.set({ text: "gone" })));
  check("the survivors closed the gap", () => c.index() === 1 && b.index() === 2);

  // Insert at the front: the new row is created, the existing two move down
  // rather than being rebuilt behind it.
  setRows(["d", "c", "b"]);
  await frame();
  check("an insert at the front keeps the rows behind it", () =>
    rowRefs.c === c && rowRefs.b === b);
  check("and puts the new row in front", () =>
    rowRefs.d.index() === 1 && c.index() === 2 && b.index() === 3);

  // Effects and the timer they own.
  const ticksBefore = intervalTicks;
  await frame();
  check("useInterval is running", () => intervalTicks > ticksBefore);

  setTicking(false);
  await frame();
  check("unmount ran the effect cleanup", () =>
    effectLog.length === 2 && effectLog[1] === "down");
  const ticksAtStop = intervalTicks;
  await frame();
  await frame();
  check("useInterval stopped with its component", () => intervalTicks === ticksAtStop);

  console.log(`UISELFTEST ${pass} passed, ${fail} failed`);
  lv.label(lv.screen(), {
    align: "bottom-mid", y: -2, font: 16,
    color: fail === 0 ? "#4CAF50" : "#FF5252",
    text: fail === 0 ? `all ${pass} checks passed` : `${fail} of ${pass + fail} FAILED`,
  });
}

run().catch(e => console.log("FAIL ui-selftest threw — " + e.message + "\n" + e.stack));
