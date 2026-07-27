// ui.js — a component runtime for the LVGL binding layer.
//
// The idea lv_binding_js is built around (describe the UI as a function of
// state and let a reconciler work out which widgets to touch) without the parts
// of it that don't fit this board: no react-reconciler in flash, no npm at
// runtime, no second C layer. This is plain JavaScript over the same `lv`
// bindings a hand-written script uses, so it costs nothing until an app asks
// for it, and an app can drop back to `lv.*` at any point.
//
// It is bundled into an app by tools/build-app.mjs; nothing on the device loads
// it. The model and its limits: docs/ui-runtime.md.
//
// The two rules the rest of the file exists to keep:
//
//   * Renders never run during LVGL event dispatch. A state write marks a
//     component dirty and returns; the re-render happens in a promise
//     microtask, which the host drains from loop() after lv_timer_handler() has
//     returned. That is what makes it safe for a click handler to replace the
//     screen it was dispatched from — the hazard every imperative script here
//     has to hand-defer around.
//   * A widget is created once and mutated afterwards. Anything that would
//     otherwise rebuild it every render (event props with a fresh identity,
//     props that did not actually change) is diffed away first.

// ---------------------------------------------------------------- elements

// Tag names are the `lv` maker names, so there is no second vocabulary to learn
// and no mapping table to keep in sync: <slider> is lv.slider().
const HOST_TAGS = ["obj", "button", "label", "slider", "switch", "arc", "list",
                   "chart", "tabview", "textarea", "keyboard"];

// Two tags that are not makers, because their widget is created by the parent
// rather than by lv.<tag>(parent):
//   <row> in a <list>     -> list.add(text)
//   <tab> in a <tabview>  -> tabview.addTab(name), and children go into the
//                            content container that returns
const PARENT_MADE = { row: "list", tab: "tabview" };

// Widgets whose own children LVGL places (a tabview holds a tab bar and a
// content pane, in that order). The runtime never reorders those, and a
// different set of tabs rebuilds the tabview rather than splicing one in —
// LVGL has no remove-tab or rename-tab to reach for.
const FIXED_ORDER = { tabview: true };

// Tags where text children set the `text` prop instead of becoming a widget.
const TEXT_PROP = { label: true, button: true, row: true };

// on<Event> prop -> the .on() event name.
const EVENTS = {
  onClick: "click", onChange: "change", onPress: "press", onPressing: "pressing",
  onLongPress: "longpress", onReady: "ready", onCancel: "cancel",
};

// Props only read when the widget is created. Changing one later does nothing,
// so the diff must not spend a .set() discovering that.
const CREATE_ONLY = { seriesColor: true, name: true };

// What a prop reverts to when it disappears between renders. Without this,
// hidden={busy} would hide a widget and never show it again: .set() ignores
// undefined, so a removed prop is indistinguishable from an absent one. Only
// props with a knowable neutral value are listed, which is why the docs say to
// pass a value rather than to omit the prop.
const RESET = { hidden: false, text: "", scroll: true, clickable: true };

const NOT_A_PROP = { key: true, ref: true, children: true };

function h(type, props, ...kids) {
  const p = {};
  for (const k in props) p[k] = props[k];
  if (kids.length) p.children = kids.length === 1 ? kids[0] : kids;
  return {
    type,
    key: p.key === undefined || p.key === null ? null : String(p.key),
    props: p,
    kids: flatten(p.children),
  };
}

// A fragment groups children without a widget of its own.
function Fragment(props) { return props.children; }

// null/undefined/false drop out (so `cond && <x/>` works), arrays splice in,
// strings and numbers survive as text.
function flatten(kids, out) {
  out = out || [];
  if (kids === undefined || kids === null || kids === false || kids === true) return out;
  if (Array.isArray(kids)) {
    for (const k of kids) flatten(k, out);
    return out;
  }
  out.push(kids);
  return out;
}

const isText = v => typeof v === "string" || typeof v === "number";

// ---------------------------------------------------------------- hooks

let current = null;   // the component instance being rendered
let hookAt = 0;

function slot(init) {
  if (!current) throw new Error("hooks can only be called while a component renders");
  const hooks = current.hooks;
  if (hooks.length <= hookAt) hooks.push(init());
  return hooks[hookAt++];
}

function useState(initial) {
  const inst = current;
  const cell = slot(() => {
    const c = { v: typeof initial === "function" ? initial() : initial };
    c.set = next => {
      const v = typeof next === "function" ? next(c.v) : next;
      // Bailing out on an unchanged value is what lets a poll loop write the
      // same reading every second and render nothing.
      if (Object.is(v, c.v)) return;
      c.v = v;
      invalidate(inst);
    };
    return c;
  });
  return [cell.v, cell.set];
}

function useRef(initial) {
  return slot(() => ({ current: initial }));
}

function useMemo(fn, deps) {
  const s = slot(() => ({ deps: null, v: undefined, first: true }));
  if (s.first || !sameDeps(s.deps, deps)) {
    s.v = fn();
    s.deps = deps;
    s.first = false;
  }
  return s.v;
}

function useCallback(fn, deps) {
  return useMemo(() => fn, deps);
}

// Runs after the widgets are in place, not during the render that asked for it.
// A returned function is the cleanup, run before the next call and once more
// when the component goes away.
function useEffect(fn, deps) {
  const inst = current;
  const s = slot(() => ({ deps: null, cleanup: null, first: true }));
  if (s.first || !sameDeps(s.deps, deps)) {
    s.deps = deps;
    s.first = false;
    inst.pending.push(s, fn);
  }
}

// An lv.timer owned by the component: stopped on unmount, restarted when the
// interval changes, and always calling the newest callback rather than the one
// that happened to be current when it started. Passing null for ms stops it.
function useInterval(fn, ms) {
  const held = useRef(fn);
  held.current = fn;
  useEffect(() => {
    if (ms === null || ms === undefined || ms === false) return undefined;
    const t = lv.timer(ms, () => held.current());
    return () => t.stop();
  }, [ms]);
}

function sameDeps(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false;
  return true;
}

// ---------------------------------------------------------------- scheduling

const dirty = [];
const effectQueue = [];
let scheduled = false;
let chain = 0;   // consecutive flushes; a runaway would otherwise hang the board

function invalidate(inst) {
  if (inst.gone || inst.dirty) return;
  inst.dirty = true;
  dirty.push(inst);
  schedule();
}

function schedule() {
  if (scheduled) return;
  scheduled = true;
  // A microtask, not lv.timer: the host drains QuickJS's job queue at the
  // bottom of loop(), so this runs after lv_timer_handler() has finished
  // dispatching and before the next frame. Nothing here is reachable from
  // inside an event callback, which is the whole safety argument.
  Promise.resolve().then(flush);
}

function flush() {
  scheduled = false;
  // jsvm_pump() drains jobs until there are none, so a component that sets
  // state every render would spin there forever with the panel frozen. Give up
  // instead, and say which component to go and look at.
  if (++chain > 40) {
    chain = 0;
    dirty.length = 0;
    effectQueue.length = 0;
    console.error("[ui] runaway render: a component sets state on every render, stopping");
    return;
  }

  while (dirty.length) {
    const inst = dirty.shift();
    if (!inst.dirty || inst.gone) continue;   // a parent may have patched it already
    inst.dirty = false;
    try {
      renderComponent(inst);
    } catch (e) {
      console.error("[ui] <" + (inst.fn.name || "anonymous") + "> failed to render:", e);
    }
  }
  runEffects();

  // Reaching a quiet point is what resets the runaway counter, so a legitimate
  // cascade (effect sets state, which renders, which schedules another effect)
  // costs nothing as long as it terminates.
  if (!scheduled) chain = 0;
}

// Effects run once the widgets exist, so an effect can read .bounds() or hand a
// widget to something else.
function runEffects() {
  while (effectQueue.length) {
    const inst = effectQueue.shift();
    const pending = inst.pending;
    inst.pending = [];
    for (let i = 0; i < pending.length; i += 2) {
      if (inst.gone) break;
      const s = pending[i], fn = pending[i + 1];
      if (s.cleanup) { safely(s.cleanup, "effect cleanup"); s.cleanup = null; }
      const r = safely(fn, "effect");
      s.cleanup = typeof r === "function" ? r : null;
    }
  }
}

function safely(fn, what) {
  try {
    return fn();
  } catch (e) {
    console.error("[ui] " + what + " threw:", e);
    return null;
  }
}

// ---------------------------------------------------------------- instances

// An instance is one node of the mounted tree. All three kinds carry `kids`, so
// the children diff has exactly one implementation:
//
//   host  a widget, plus whatever it contains
//   comp  a function component; kids is what it returned
//   frag  no widget of its own; the render root is one
//
// Two fields decide where a child's widget goes:
//
//   box    the lv widget children attach to
//   owner  the instance that owns `box` — a host, or the root. comp instances
//          pass both through unchanged, which is why a component can render one
//          element or five without the parent knowing the difference.

function mount(vnode, box, owner) {
  if (isText(vnode)) {
    // A bare string where a widget belongs gets one, so <obj>hi</obj> renders.
    const inst = mountHost(h("label", { text: String(vnode) }), box, owner);
    inst.autoLabel = true;
    return inst;
  }
  if (typeof vnode.type === "function") {
    const inst = { kind: "comp", vnode, fn: vnode.type, key: vnode.key, hooks: [],
                   pending: [], kids: [], box, owner, dirty: false, mounted: false,
                   gone: false };
    renderComponent(inst);
    return inst;
  }
  return mountHost(vnode, box, owner);
}

function mountHost(vnode, box, owner) {
  const tag = vnode.type;
  const props = vnode.props;
  const inst = { kind: "host", vnode, key: vnode.key, tag, box, owner,
                 kids: [], handlers: {}, bound: {}, autoLabel: false, gone: false };

  const create = {};
  for (const k in props) {
    if (NOT_A_PROP[k] || EVENTS[k]) continue;
    create[k] = props[k];
  }
  const text = textOf(vnode);
  if (text !== null) create.text = text;

  if (PARENT_MADE[tag]) {
    if (!owner || owner.tag !== PARENT_MADE[tag])
      throw new Error("<" + tag + "> must be a direct child of <" + PARENT_MADE[tag] + ">");
    // The parent applies one prop as it creates the widget; the rest still has
    // to go on afterwards.
    if (tag === "row") {
      inst.w = owner.w.add(create.text || "");
      delete create.text;
    } else {
      inst.w = owner.w.addTab(String(props.name || ""));
      delete create.name;
    }
    if (Object.keys(create).length) inst.w.set(create);
  } else {
    if (HOST_TAGS.indexOf(tag) < 0) throw new Error("unknown element <" + tag + ">");
    inst.w = lv[tag](box, create);
  }

  bindEvents(inst, props);
  setRef(props.ref, inst.w);

  // A widget whose text came from its children has no child widgets.
  if (text === null) inst.kids = mountKids(vnode.kids, inst.w, inst);
  return inst;
}

function mountKids(kids, box, owner) {
  const out = [];
  for (const k of kids) out.push(mount(k, box, owner));
  return out;
}

// A component's own render: call it, then reconcile what it returned against
// what it returned last time. Everything below that is the ordinary children
// diff, so a component rendering one element and one rendering three are the
// same case.
function renderComponent(inst) {
  const prevInst = current, prevHook = hookAt;
  current = inst;
  hookAt = 0;
  let out;
  try {
    out = inst.fn(inst.vnode.props);
  } finally {
    current = prevInst;
    hookAt = prevHook;
  }
  const kids = flatten(out);
  if (inst.mounted) {
    diffKids(inst, kids);
  } else {
    inst.mounted = true;
    inst.kids = mountKids(kids, inst.box, inst.owner);
  }
  if (inst.pending.length) effectQueue.push(inst);
}

function unmount(inst) {
  inst.gone = true;
  if (inst.kind === "comp") {
    for (const s of inst.hooks) {
      if (s && s.cleanup) { safely(s.cleanup, "effect cleanup"); s.cleanup = null; }
    }
    inst.pending.length = 0;
  }
  for (const k of inst.kids) unmount(k);
  if (inst.kind === "host") {
    setRef(inst.vnode.props.ref, null);
    // Deleting the widget fires LV_EVENT_DELETE, which is what releases the
    // event trampolines the C layer holds for it. Its child widgets go with it,
    // so the walk above is only for their refs and effect cleanups.
    inst.w.delete();
  }
}

function setRef(ref, value) {
  if (!ref) return;
  if (typeof ref === "function") safely(() => ref(value), "ref");
  else ref.current = value;
}

// ---------------------------------------------------------------- events

// One trampoline per widget per event, registered at most once and never
// removed. It reads the handler back through the instance at dispatch time, so
// a fresh arrow function every render costs nothing and there is no need for an
// .off() binding: the C layer goes on holding the same closure for the widget's
// life, and releases it with the widget.
function bindEvents(inst, props) {
  for (const prop in EVENTS) {
    if (typeof props[prop] !== "function") continue;
    inst.handlers[prop] = props[prop];
    if (inst.bound[prop]) continue;
    inst.bound[prop] = true;
    const name = EVENTS[prop];
    inst.w.on(name, (w, x, y) => {
      const fn = inst.handlers[prop];
      if (!fn) return;
      const e = { target: w, type: name, x, y };
      if (name === "change") e.value = w.value();
      fn(e);
    });
  }
}

function updateEvents(inst, props) {
  for (const prop in EVENTS) {
    const fn = props[prop];
    inst.handlers[prop] = typeof fn === "function" ? fn : null;
  }
  bindEvents(inst, props);
}

// ---------------------------------------------------------------- diffing

function textOf(vnode) {
  if (!TEXT_PROP[vnode.type]) return null;
  if (vnode.props.text !== undefined) return String(vnode.props.text);
  if (!vnode.kids.length) return null;
  if (!vnode.kids.every(isText)) return null;
  return vnode.kids.join("");
}

// Only props that actually changed reach .set(). This is the difference between
// a re-render and a rebuild: a label whose colour and size are written on every
// render still costs one lv_label_set_text and nothing else.
function diffProps(inst, oldVnode, newVnode) {
  const oldProps = oldVnode.props, newProps = newVnode.props;
  const patchProps = {};
  let any = false;

  for (const k in newProps) {
    if (NOT_A_PROP[k] || EVENTS[k] || CREATE_ONLY[k] || k === "text") continue;
    if (!eq(newProps[k], oldProps[k])) { patchProps[k] = newProps[k]; any = true; }
  }
  for (const k in oldProps) {
    if (NOT_A_PROP[k] || EVENTS[k] || CREATE_ONLY[k] || k === "text") continue;
    if (k in newProps || !(k in RESET)) continue;
    patchProps[k] = RESET[k];
    any = true;
  }

  const oldText = textOf(oldVnode), newText = textOf(newVnode);
  if (newText !== null && newText !== oldText) { patchProps.text = newText; any = true; }
  else if (newText === null && oldText !== null) { patchProps.text = RESET.text; any = true; }

  if (any) inst.w.set(patchProps);
}

// Shallow, with one level for arrays: range={[0, 100]} and divs={[3, 5]} are a
// fresh array on every render and would otherwise rewrite the widget each time.
function eq(a, b) {
  if (Object.is(a, b)) return true;
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false;
    return true;
  }
  return false;
}

// Can this instance be updated in place, or does it have to be replaced?
function compatible(inst, vnode) {
  if (isText(vnode)) return inst.kind === "host" && inst.autoLabel;
  if (vnode.key !== inst.key) return false;
  if (typeof vnode.type === "function") return inst.kind === "comp" && inst.fn === vnode.type;
  if (inst.kind !== "host" || inst.tag !== vnode.type || inst.autoLabel) return false;
  // A tabview's tabs are created by LVGL and cannot afterwards be added,
  // removed or renamed, so a different set of them is a different widget.
  if (FIXED_ORDER[vnode.type]) return sameTabs(inst.vnode.kids, vnode.kids);
  if (inst.tag === "tab") return String(inst.vnode.props.name || "") === String(vnode.props.name || "");
  return true;
}

function sameTabs(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    const x = a[i], y = b[i];
    if (isText(x) || isText(y) || x.type !== y.type) return false;
    if (x.type === "tab" && String(x.props.name || "") !== String(y.props.name || "")) return false;
  }
  return true;
}

function patch(inst, vnode) {
  if (isText(vnode)) vnode = h("label", { text: String(vnode) });

  if (inst.kind === "comp") {
    inst.vnode = vnode;
    inst.dirty = false;
    renderComponent(inst);
    return inst;
  }

  const oldVnode = inst.vnode;
  inst.vnode = vnode;
  diffProps(inst, oldVnode, vnode);
  updateEvents(inst, vnode.props);
  if (oldVnode.props.ref !== vnode.props.ref) {
    setRef(oldVnode.props.ref, null);
    setRef(vnode.props.ref, inst.w);
  }
  if (textOf(vnode) === null) diffKids(inst, vnode.kids);
  return inst;
}

// The children diff. Two modes, chosen by whether anything carries a key:
//
//   positional  index i is matched against index i. Nothing ever moves, so a
//               list that grows or shrinks at the end costs nothing.
//   keyed       matched by key, and a surviving widget is moved rather than
//               rebuilt — the case where an insert at the front would otherwise
//               rewrite every row behind it.
function diffKids(ownerInst, newKids) {
  const old = ownerInst.kids;
  const box = ownerInst.kind === "host" ? ownerInst.w : ownerInst.box;
  const owner = ownerInst.kind === "host" ? ownerInst : ownerInst.owner;
  const keyed = old.some(i => i.key !== null) ||
                newKids.some(v => !isText(v) && v.key !== null);

  const next = [];
  let structural = false;

  if (!keyed) {
    for (let i = 0; i < newKids.length; i++) {
      const o = old[i];
      if (o && compatible(o, newKids[i])) {
        next.push(patch(o, newKids[i]));
      } else {
        if (o) unmount(o);
        next.push(mount(newKids[i], box, owner));
        structural = true;
      }
    }
    for (let i = newKids.length; i < old.length; i++) { unmount(old[i]); structural = true; }
  } else {
    // Keyed by type first, then by key, rather than by one composite string.
    // A component's type is the function itself: concatenating that would
    // stringify its entire source on every diff, and would collide two
    // components that happen to be written identically.
    const pool = new Map();
    for (const o of old) {
      if (o.key === null) continue;
      const type = tagOf(o);
      let byKey = pool.get(type);
      if (!byKey) pool.set(type, byKey = new Map());
      byKey.set(o.key, o);
    }
    const reused = new Set();
    let unkeyedAt = 0;
    for (const v of newKids) {
      const key = isText(v) ? null : v.key;
      let o = null;
      if (key !== null) {
        const byKey = pool.get(typeOf(v));
        o = (byKey && byKey.get(key)) || null;
      } else {
        // Unkeyed children mixed in with keyed ones still match positionally,
        // against the unkeyed olds in order.
        while (unkeyedAt < old.length && old[unkeyedAt].key !== null) unkeyedAt++;
        o = old[unkeyedAt] || null;
        unkeyedAt++;
      }
      if (o && !reused.has(o) && compatible(o, v)) {
        reused.add(o);
        next.push(patch(o, v));
      } else {
        next.push(mount(v, box, owner));
        structural = true;
      }
    }
    for (const o of old) {
      if (!reused.has(o)) { unmount(o); structural = true; }
    }
    if (!structural) {
      for (let i = 0; i < next.length; i++) {
        if (next[i] !== old[i]) { structural = true; break; }
      }
    }
  }

  ownerInst.kids = next;

  // Whatever was created, removed or replaced landed at the end of the parent's
  // child list rather than where the tree says it belongs, so put the widgets
  // back into declaration order. Skipped where LVGL owns the order.
  if (structural && owner && !FIXED_ORDER[owner.tag]) reorder(owner, box);
}

function tagOf(inst) { return inst.kind === "comp" ? inst.fn : inst.tag; }
function typeOf(vnode) { return isText(vnode) ? "label" : vnode.type; }

// Walk the widgets this container holds, in tree order, and give each the index
// it should have. Cheap at the widget counts a 172x320 panel holds, and it is
// what makes a keyed reorder a move instead of a rebuild.
//
// It assumes every child of `box` came from this tree, which render() arranges
// by emptying the root before it mounts anything.
function reorder(owner, box) {
  const widgets = [];
  collect(owner, widgets, box);
  for (let i = 0; i < widgets.length; i++) {
    if (widgets[i].index() !== i) widgets[i].index(i);
  }
}

function collect(inst, out, box) {
  for (const k of inst.kids) {
    if (k.kind === "host") {
      if (k.box === box) out.push(k.w);
    } else {
      collect(k, out, box);
    }
  }
}

// ---------------------------------------------------------------- entry point

// render(tree, parent?) — mount a tree and keep it up to date.
//
// The parent becomes the runtime's: it is emptied first, because ordering
// assumes every child under it came from this tree. Returns a handle with
// .update() for a tree that is redrawn from outside (a plain script driving it
// without components) and .unmount(), which apps rarely need since a reload
// destroys the VM anyway.
function render(tree, parent) {
  const box = parent || lv.screen();
  box.clean();
  const root = { kind: "frag", key: null, vnode: null, kids: [], box, owner: null,
                 gone: false };
  root.owner = root;   // the root owns its own box, so reorder() has a start point
  root.kids = mountKids(flatten(tree), box, root);
  reorder(root, box);
  runEffects();
  return {
    update(next) { diffKids(root, flatten(next)); runEffects(); },
    unmount() { for (const k of root.kids) unmount(k); root.kids = []; },
  };
}

// The public surface, and the last statement in the file for a reason:
// tools/build-app.mjs wraps everything above in a function and returns this, so
// an app gets these nine names and none of the internals. Running the file as
// it stands (the host tests do) just leaves UI defined.
const UI = { h, Fragment, render, useState, useEffect, useRef, useMemo, useCallback, useInterval };
