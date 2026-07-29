// tasks.jsx — a checklist, and the worked example for the component runtime.
//
// Build it before pushing (app/apps/tasks.js is generated, not edited):
//
//   node tools/build-app.mjs tasks
//   .\push.ps1 app\apps\tasks.js
//
// It is here to show the parts of the model that a counter would not:
//
//   Done items sink to the bottom, so every toggle reorders the list. The rows
//   carry keys, so the widgets move rather than being rebuilt — the case an
//   imperative version has to handle by deleting every row and making new ones,
//   which is also the case where deleting the row LVGL is dispatching to would
//   bite. Renders are batched into a microtask, so it cannot.
//
//   The confirm panel replaces the screen's contents from inside a click
//   handler, with none of the lv.timer(20) deferral apps/wifi.js needs.
//
//   Nothing here calls .clean(), .delete(), or .set(). The tree is the whole
//   description of the screen, and everything else is the runtime's problem.

const STORE = "/tasks.json";

const SEED = [
  { id: 1, text: "Flash the firmware", done: true },
  { id: 2, text: "Push an app", done: true },
  { id: 3, text: "Write one in JSX", done: false },
  { id: 4, text: "Reorder without a rebuild", done: false },
  { id: 5, text: "Read docs/ui-runtime.md", done: false },
];

function load() {
  if (!fs.available()) return SEED;
  try {
    const raw = fs.read(STORE);
    if (!raw) return SEED;
    const items = JSON.parse(raw);
    return Array.isArray(items) && items.length ? items : SEED;
  } catch (e) {
    console.error("tasks: could not read " + STORE + ":", e);
    return SEED;
  }
}

function save(items) {
  if (!fs.available()) return;
  fs.write(STORE, JSON.stringify(items));
}

const S = lv.size();
// The firmware's corner button is 34px in the bottom-right; leave it clear.
const CORNER = 40;
const HEADER = 34;
const FOOTER = 38;

const INK = "#F0F4F8";
const DIM = "#64798C";
const CARD = "#101E2C";

function Header({ done, total }) {
  return (
    <obj w="100%" h={HEADER} bg="#0B1622" border={0} pad={0} radius={0} scroll={false}
         align="top-mid" y={0}>
      <label align="left-mid" x={10} font={20} color={INK} text="Tasks" />
      <label align="right-mid" x={-10} font={14} color={DIM} text={done + "/" + total + " done"} />
    </obj>
  );
}

function TaskRow({ task, onToggle }) {
  // A row is a list button, so its widget comes from the list rather than from
  // lv.button(); that is the whole difference, and the key is what lets it be
  // moved instead of rebuilt when this task sinks past the others.
  return (
    <row key={task.id}
         text={(task.done ? "✓  " : "·  ") + task.text}
         color={task.done ? DIM : INK}
         onClick={() => onToggle(task.id)} />
  );
}

function Confirm({ count, onYes, onNo }) {
  return (
    <obj w="88%" h="content" align="center" bg={CARD} border={1} borderColor="#2A4258"
         radius={10} pad={12} flex="column" flexAlign={["center", "center"]} scroll={false}>
      <label w="100%" font={16} color={INK} text={"Clear " + count + " done?"} />
      <obj w="100%" h="content" bg={CARD} border={0} pad={0} radius={0} scroll={false}
           flex="row" flexAlign={["evenly", "center"]}>
        <button w={64} h={34} bg="#1D3A57" color={INK} text="Clear" onClick={onYes} />
        <button w={64} h={34} bg="#22303E" color={INK} text="Keep" onClick={onNo} />
      </obj>
    </obj>
  );
}

function App() {
  const [items, setItems] = useState(load);
  const [asking, setAsking] = useState(false);

  // Undone first, and stable within each group, so a toggle moves exactly one
  // row. The list this returns is what the reconciler diffs against the last
  // one; the keys are what make the difference a move.
  const ordered = useMemo(
    () => [...items].sort((a, b) => (a.done ? 1 : 0) - (b.done ? 1 : 0)),
    [items],
  );
  const done = items.filter(t => t.done).length;

  // Writing to storage is a side effect of the list changing, not of the tap
  // that changed it — so it happens once per change however the change arrived.
  useEffect(() => { save(items); }, [items]);

  const toggle = id =>
    setItems(items.map(t => (t.id === id ? { ...t, done: !t.done } : t)));

  return (
    <>
      <Header done={done} total={items.length} />

      <list w={S.w - 12} h={S.h - HEADER - FOOTER} align="top-mid" y={HEADER}
            bg={CARD} border={0} radius={8}>
        {ordered.map(task => <TaskRow key={task.id} task={task} onToggle={toggle} />)}
      </list>

      {!asking && done > 0 && (
        <button w={92} h={30} align="bottom-left" x={8} y={-4} bg="#22303E" color={INK}
                font={14} text="Clear done" onClick={() => setAsking(true)} />
      )}

      {asking && (
        <Confirm
          count={done}
          onYes={() => { setItems(items.filter(t => !t.done)); setAsking(false); }}
          onNo={() => setAsking(false)}
        />
      )}
    </>
  );
}

lv.screen().set({ bg: "#0B1622" });
render(<App />);
