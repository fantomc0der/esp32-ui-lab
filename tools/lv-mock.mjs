// lv-mock.mjs — the `lv` binding surface, in Node, for testing app/lib/ui.js.
//
// The real test of anything on this board is app/selftest.js on the panel, and
// that is still true here. What this buys is the half CI can run: the runtime's
// reconciliation is pure bookkeeping over `lv` calls, so a fake `lv` that
// records those calls proves the bookkeeping without a board — that a re-render
// moved a row instead of rebuilding it, that a removed child was deleted
// exactly once, that an unchanged prop cost no .set().
//
// It mirrors the C layer's behaviour where that behaviour is load-bearing:
//
//   * A handle to a deleted widget throws, the way jsvm_arg_widget() does with
//     lv_obj_is_valid(). A runtime bug that writes through a stale handle fails
//     here instead of corrupting the heap on the panel.
//   * .set() ignores undefined and null, so "the prop was removed" and "the
//     prop was never there" are indistinguishable — the reason the runtime
//     carries a RESET table.
//   * A button's `text` prop creates a label child, so widget counts match.

// A memory-backed `fs`, enough for an app that keeps a little state. Same
// contract as bindings_fs.cpp: read() gives the text or null, write() gives a
// boolean, and available() is how a script finds out whether to bother.
export function makeFs(files = {}, dirs = {}) {
  return {
    files,
    dirs,
    available: () => true,
    read: p => (p in files ? files[p] : null),
    write: (p, text) => { files[p] = String(text); return true; },
    append: (p, text) => { files[p] = (files[p] || "") + String(text); return true; },
    exists: p => p in files,
    remove: p => { delete files[p]; return true; },
    mkdir: p => { dirs[p] = dirs[p] || []; return true; },
    isDir: p => p in dirs,
    // Bare names, and null for anything that is not a directory — the same
    // shape bindings_fs.cpp returns, because a script that checks for null is
    // relying on it.
    list: p => (p in dirs ? dirs[p].slice() : null),
  };
}

// `sys` and `wifi`, fixed so two runs are comparable. The readings are frozen
// rather than live: a parity test between two versions of an app needs both to
// see the same numbers, or every label differs for reasons that have nothing to
// do with the port.
export function makeSys(over = {}) {
  return {
    heap: () => ({ internal: 210 * 1024, psram: 7900 * 1024 }),
    fps: () => 27,
    uptime: () => 125000,
    battery: () => 4.02,
    backlight: pct => pct,
    info: () => ({ model: "ESP32-S3", rev: 0, cores: 2, mhz: 240, flashMB: 16,
                   psramMB: 8, lvgl: "9.5.0", quickjs: "0.15.1" }),
    launch: () => true,
    pin: () => true,
    unpin: () => true,
    pinned: () => null,
    ...over,
  };
}

export function makeWifi(nets = [{ ssid: "home", rssi: -52, open: false },
                                 { ssid: "guest", rssi: -71, open: true }]) {
  const pending = [];
  return {
    status: () => ({ connected: true, ssid: "home", ip: "10.0.0.4", rssi: -52, saved: true }),
    save: () => true,
    connect: () => true,
    forget: () => true,
    // Async in the real binding: the callback lands on a later tick, which is
    // what an app has to cope with. deliver() is how a test runs that tick.
    scan: fn => { pending.push(fn); return true; },
    deliver: () => { while (pending.length) pending.shift()(nets); },
  };
}

function dumpTree(node, indent) {
  let out = "";
  for (const k of node.kids) {
    if (k.synthetic) continue;
    const bits = [];
    if (k.props.text !== undefined) bits.push(JSON.stringify(k.props.text));
    if (k.props.name !== undefined) bits.push("name=" + JSON.stringify(k.props.name));
    out += `${indent}${k.tag}${bits.length ? " " + bits.join(" ") : ""}\n`;
    out += dumpTree(k, indent + "  ");
  }
  return out;
}

export function makeLv() {
  const stats = { created: 0, deleted: 0, sets: 0, moves: 0, handlers: 0 };
  // Every .set() after the widget was created, as {tag, keys}. Counting calls
  // is not enough to show the diff works: sending every prop every time is
  // still one call, and it is exactly the thing the diff exists to avoid.
  const patches = [];
  let nextId = 1;

  const die = () => { throw new TypeError("widget has been deleted"); };

  function makeWidget(tag, parent, props) {
    const w = {
      id: nextId++,
      tag,
      parent,
      kids: [],
      props: {},
      handlers: {},   // event -> [fn]
      dead: false,
    };

    const live = () => { if (w.dead) die(); return w; };

    w.set = p => {
      live();
      stats.sets++;
      if (w.born) patches.push({ tag: w.tag, keys: Object.keys(p).sort() });
      for (const k in p) {
        // The C layer's `has()` check: undefined and null mean "not given".
        if (p[k] === undefined || p[k] === null) continue;
        w.props[k] = p[k];
        if (k === "text" && (tag === "button" || tag === "row")) ensureLabel(w, p[k]);
      }
      return w;
    };

    w.on = (event, fn) => {
      live();
      stats.handlers++;
      (w.handlers[event] ??= []).push(fn);
      return w;
    };

    w.value = v => {
      live();
      if (v === undefined) return w.props.value;
      w.props.value = v;
      return w;
    };

    w.add = text => {
      live();
      if (tag !== "list") throw new TypeError("add() only works on lv.list widgets");
      return makeWidget("row", w, { text });
    };

    w.addTab = name => {
      live();
      if (tag !== "tabview") throw new TypeError("addTab() only works on lv.tabview widgets");
      return makeWidget("tab", w, { name });
    };

    w.push = () => { live(); return w; };
    w.target = () => { live(); return w; };
    w.bounds = () => { live(); return { x: 0, y: 0, w: 172, h: 320 }; };

    w.clean = () => {
      live();
      for (const k of [...w.kids]) destroy(k);
      w.kids = [];
      return w;
    };

    w.delete = () => {
      live();
      if (!w.parent) throw new TypeError("the screen cannot be deleted (use .clean())");
      const at = w.parent.kids.indexOf(w);
      if (at >= 0) w.parent.kids.splice(at, 1);
      destroy(w);
      return undefined;
    };

    w.index = n => {
      live();
      if (!w.parent) return 0;
      const at = w.parent.kids.indexOf(w);
      if (n === undefined) return at;
      if (n !== at) {
        stats.moves++;
        w.parent.kids.splice(at, 1);
        w.parent.kids.splice(n, 0, w);
      }
      return w;
    };

    if (parent) parent.kids.push(w);
    stats.created++;
    if (props) w.set(props);
    w.born = true;
    return w;
  }

  // A button given text grows a label child, exactly as widget_set_text() does,
  // so a test counting widgets sees what the panel would hold.
  function ensureLabel(w, text) {
    let label = w.kids.find(k => k.tag === "label" && k.synthetic);
    if (!label) {
      label = makeWidget("label", w, null);
      label.synthetic = true;
    }
    label.props.text = text;
  }

  function destroy(w) {
    w.dead = true;
    stats.deleted++;
    for (const k of w.kids) destroy(k);
    w.kids = [];
  }

  const screen = makeWidget("screen", null, null);
  stats.created = 0;   // the screen is the fixture, not something a test made

  const timers = new Set();

  const lv = {
    screen: () => screen,
    size: () => ({ w: 172, h: 320 }),
    timer: (ms, fn) => {
      const t = { ms, fn, stopped: false };
      t.stop = () => { t.stopped = true; timers.delete(t); };
      timers.add(t);
      return t;
    },
  };
  for (const tag of ["obj", "button", "label", "slider", "switch", "arc", "list",
                     "chart", "tabview", "textarea", "keyboard", "bar", "checkbox",
                     "roller", "dropdown", "spinner", "led"]) {
    lv[tag] = (parent, props) => makeWidget(tag, parent, props);
  }

  return {
    lv,
    screen,
    stats,
    patches,
    timers,
    // Fire every timer once, the way lv_timer_handler() would.
    tick: () => { for (const t of [...timers]) if (!t.stopped) t.fn(); },
    // Dispatch an event the way event_trampoline() does: the widget first, then
    // the touch point.
    fire: (w, event, x = 0, y = 0) => {
      for (const fn of w.handlers[event] || []) fn(w, x, y);
    },
    // The widget tree as nested plain text, for readable assertions. The label
    // a button grows for its own text is left out: it is the C layer's, not
    // something the tree under test asked for.
    dump: (w = screen, indent = "") => {
      let out = "";
      for (const k of w.kids) {
        if (k.synthetic) continue;
        const bits = [];
        if (k.props.text !== undefined) bits.push(JSON.stringify(k.props.text));
        if (k.props.name !== undefined) bits.push("name=" + JSON.stringify(k.props.name));
        out += `${indent}${k.tag}${bits.length ? " " + bits.join(" ") : ""}\n`;
        out += dumpTree(k, indent + "  ");
      }
      return out;
    },
  };
}
