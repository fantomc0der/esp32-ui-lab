# The component runtime

An optional way to write apps: describe the screen as a function of state, and let a reconciler work out which widgets to touch. It is JSX and a small set of React's hooks, implemented in [`app/lib/ui.js`](../app/lib/ui.js) — about 400 lines of plain JavaScript over the same `lv` bindings a hand-written script uses.

Nothing in the firmware knows it exists. It is not a second API; it is a library that calls the first one. An app can use it, ignore it, or mix the two.

## Why this is here, given that the docs said it wasn't

[`design-rationale.md`](design-rationale.md) explains at length why this project is not an lvgljs port, and lists "no JSX, no React component model" among the costs. That list was about **lvgljs's implementation**, which puts the component model in C, underneath txiki.js, libuv and curl, and reaches the screen through a Linux framebuffer. None of that fits a FreeRTOS microcontroller, and none of it was ever going to.

The *idea* was always separable from that, and this is the idea on its own: no C, no flash, no npm, no dependency the board has to carry. The firmware is unchanged except for two widget methods (`.delete()` and `.index()`), together about twenty lines, both useful to imperative scripts too. An app that does not use the runtime does not pay for it, because the runtime only exists inside the apps that bundle it.

So the honest version of the old claim is narrower than it was, and still true where it counts: **the firmware has no JSX, no React and no virtual DOM.** The app layer now can.

## The five-minute version

```jsx
function Counter() {
  const [n, setN] = useState(0);
  return (
    <obj w="100%" h="100%" flex="column" flexAlign="center">
      <label font={40} color="#F0F4F8" text={String(n)} />
      <button w={90} h={40} onClick={() => setN(n + 1)}>Add one</button>
    </obj>
  );
}

render(<Counter />);
```

Build it, then push the result:

```powershell
node tools/build-app.mjs counter     # app/src/counter.jsx -> app/apps/counter.js
.\push.ps1 app\apps\counter.js
```

[`app/src/tasks.jsx`](../app/src/tasks.jsx) is the worked example, and deliberately covers what a counter does not: a keyed list that reorders on every toggle, storage as an effect, and a panel that replaces the screen from inside a click handler.

## What it buys

**A screen you can read.** The layout is one expression rather than a sequence of `lv.*` calls plus the bookkeeping to update them. Compare [`apps/vitals.js`](../app/apps/vitals.js), which keeps eight widget handles alive in module scope so a timer can write to them, against a component that just returns a different tree.

**Updates you don't have to write.** `.set()` calls, `.clean()`, which handle to keep, which to throw away: all of that becomes the reconciler's problem. It sends only the props that changed — a label whose colour, font and alignment are written on every render still costs exactly one `lv_label_set_text`.

**Reordering without rebuilding.** Give a list's children `key` props and a reshuffle moves the existing widgets (`lv_obj_move_to_index`) instead of deleting and rebuilding them. Rebuilding is what an imperative version does, because working out the minimal set of moves by hand is not worth it for one screen.

**The click-handler hazard, gone.** [`binding-api.md`](binding-api.md) warns that rebuilding the screen from inside a click handler deletes the widget LVGL is dispatching to, and tells you to defer by a tick — the `lv.timer(20, ...)` dance in [`apps/wifi.js`](../app/apps/wifi.js). Under the runtime, a state write never renders immediately. It marks the component dirty and returns; the render happens in a promise microtask, which the host drains from `loop()` *after* `lv_timer_handler()` has returned. There is nothing to defer, because nothing was ever going to run during dispatch.

## What it costs

About 8 KB of source bundled into every app that uses it, and a build step. Renders happen one frame later than an imperative `.set()` would, which is invisible at UI rates and is the price of the batching above. And there is now a generated file in the tree: `app/apps/*.js` built from `app/src/*.jsx`, committed because it is what the card holds.

It is not free of the panel, either. Fonts do not scale, the corner button still occupies the bottom-right 40 pixels, and `lv.size()` is still how you find out how tall the display is. Everything in [binding-api.md's layout guidance](binding-api.md#writing-for-more-than-one-screen) applies unchanged; components change how a layout is expressed, not what a layout has to reckon with.

## Elements

Tag names are the `lv` maker names, so there is no second vocabulary and nothing to keep in sync: `<slider>` is `lv.slider()`, and every prop in [binding-api.md's props table](binding-api.md#props) works exactly as it does there.

| Tag | |
|---|---|
| `<obj>` `<button>` `<label>` `<slider>` `<switch>` `<arc>` `<list>` `<chart>` `<tabview>` `<textarea>` `<keyboard>` `<bar>` `<checkbox>` `<roller>` `<dropdown>` `<spinner>` `<led>` | the seventeen makers |
| `<row>` | a list row. Only valid directly inside a `<list>`, because the widget comes from `list.add(text)` rather than from a maker |
| `<tab name="…">` | a tabview tab, and likewise only inside a `<tabview>`. Its children go into the content container `addTab()` returns |
| `<>…</>` | a fragment: several elements where one is expected, with no container widget |

A capitalised name is a component, per JSX's own rule: `<Header />` calls the function `Header` in scope with the props as its argument.

Text children set the `text` prop on the tags that have one (`<label>`, `<button>`, `<row>`, `<checkbox>`), so `<button>Scan</button>` and `<button text="Scan" />` are the same thing. A string anywhere else becomes a `<label>`, which is what makes `<obj>{message}</obj>` render.

Adding a tag is a firmware change, not a runtime one: a tag is only a name for an `lv` maker, so `<image>` cannot exist until `lv.image()` does. `tools/check-js-api.mjs` enforces exactly that, failing the build if the runtime lists a tag the C layer has no maker for.

### Event props

`onClick`, `onChange`, `onPress`, `onPressing`, `onLongPress`, `onReady`, `onCancel` — the seven `.on()` events. The handler receives one object: `{ target, type, x, y }`, plus `value` on a change event, where `target` is the widget handle and `x`/`y` are the touch point in screen coordinates when a pointer drove the event.

Passing a fresh arrow function on every render is free and expected. The runtime registers one trampoline per widget per event and swaps the function it reads, so there is never a second `.on()` and never a stale closure.

### key and ref

`key` identifies a child across renders. Without one, children are matched by position, which is right for a fixed layout and wrong for a list that reorders. With one, a child that moves takes its widget with it.

`ref` gives you the widget handle itself, for the things a tree cannot express: `chart.push(n)`, `keyboard.target(field)`, reading `textarea.value()`, measuring with `.bounds()`. Pass an object from `useRef(null)` and read `.current`, or pass a function to be called with the widget (and with `null` when it goes away).

```jsx
const chart = useRef(null);
useInterval(() => chart.current?.push(sys.heap().internal >> 10), 1000);
return <chart ref={chart} points={40} w="100%" h={80} />;
```

## Hooks

`useState`, `useEffect`, `useRef`, `useMemo`, `useCallback` behave as they do in React, with React's rules: call them unconditionally, in the same order, at the top of a component.

Two things differ from React, both because of what this board is:

- **`useState` bails out on an unchanged value.** Writing the same reading every second renders nothing at all, which is what a polling app does most of the time.
- **`useInterval(fn, ms)` replaces `setInterval`, which does not exist here.** It owns an `lv.timer`: stopped when the component goes away, restarted when `ms` changes, and always calling the newest `fn` rather than the one that happened to be current when the timer started. Pass `null` for `ms` to stop it.

```jsx
function Vitals() {
  const [heap, setHeap] = useState(0);
  useInterval(() => setHeap(sys.heap().psram), 1000);
  return <label text={(heap >> 10) + " KB free"} />;
}
```

There is no `useContext`, no `useReducer`, no class components, no error boundaries, no portals, no Suspense, and no `React.memo`. A component that throws is reported over serial and the rest of the tree keeps running.

## The build step

The board parses plain ES2023, so JSX has to be gone before a script is pushed. [`tools/build-app.mjs`](../tools/build-app.mjs) does the transform and the bundling, with no dependencies — CI here runs `node --check` and a couple of scripts, and adding an npm tree to keep that working was not worth it.

```powershell
node tools/build-app.mjs             # every app/src/*.jsx
node tools/build-app.mjs tasks       # just one
node tools/build-app.mjs --check     # fail if a committed output is stale (CI runs this)
```

The output is one self-contained file: the app wrapped in a function, then the runtime, then the call. That ordering exists for line numbers. The runtime is a couple of hundred lines even with its comments stripped, and it cannot be one line, because the host truncates a serial line past 4 KB (`kMaxLine` in `jsvm_app.cpp`). Putting it first would push every line of the app down by that much, and a stack trace off the board would point nowhere useful. This way the offset is a constant three lines, and [`tools/jsx.mjs`](../tools/jsx.mjs) puts each newline back where the source had it, so `/apps/tasks.js:118` is line 115 of `tasks.jsx`.

Two consequences worth knowing. `app/apps/*.js` is generated and committed, so editing one directly is work you will lose. And a built app's top-level names live inside a function, so the serial REPL can no longer reach into an app's variables the way it can with a hand-written script.

### What the transform does not handle

Entity escapes (`&amp;`), namespaced attributes (`xlink:href`), and TypeScript. A regular-expression literal containing an unbalanced brace or quote, written inside a JSX expression container, will also confuse the scanner — hoist it to a `const` outside the JSX. [`tools/test-jsx.mjs`](../tools/test-jsx.mjs) covers the cases that matter, including the ones that only look like JSX (`a < b`, `/<label>/`, a tag inside a string or a comment).

## How the reconciler works, and where it stops

Each element becomes an instance holding its widget. On re-render, an instance is reused when its type and `key` match, which means a `.set()` with the changed props; otherwise it is deleted and replaced. Children are matched by position when nothing carries a key, and by key when anything does.

Anything created, removed or replaced lands at the end of its parent's child list rather than where the tree says it belongs, so a structural change is followed by a pass that walks the container's widgets and assigns `.index()`. That is why **`render()` empties the parent it is given**: the ordering pass assumes every child under it came from this tree.

Three limits are real and worth reading before you hit them:

**A prop that disappears usually keeps its last value.** `.set()` ignores `undefined`, so "removed" and "never there" are indistinguishable at the binding layer. The runtime carries a small table of props whose neutral value is knowable (`hidden`, `text`, `scroll`, `clickable`) and resets those; everything else stays. Pass a value rather than omitting the prop: `bg={active ? BLUE : GREY}`, not `{...(active && { bg: BLUE })}`.

**Tabviews are rebuilt when their tabs change.** LVGL has no remove-tab or rename-tab, so a different set of `<tab>` names replaces the whole `<tabview>`. Tab *contents* update in place as normal. Keep the tab set static and this never comes up.

**`seriesColor` is read once.** It is set when the chart's series is created, so changing it later does nothing. The runtime knows this and does not waste a `.set()` discovering it.

## Testing without a board

The reconciler is pure bookkeeping over `lv` calls, so a fake `lv` proves the bookkeeping. [`tools/lv-mock.mjs`](../tools/lv-mock.mjs) is that fake, and it mirrors the C layer where the behaviour is load-bearing: a handle to a deleted widget throws the way `jsvm_arg_widget()` does, `.set()` ignores `undefined`, and a button given `text` grows a label child.

```powershell
node tools/test-jsx.mjs     # the transform
node tools/test-ui.mjs      # the runtime, and the built tasks app end to end
```

The assertions that matter are about what the runtime *did not* do: a re-render that creates no widgets, a reorder that moves rather than rebuilds, a handler swapped without a second `.on()`. Counting calls is not enough — sending every prop every time is still one `.set()` — so the mock records what was in each one.

This does not replace [`app/selftest.js`](../app/selftest.js), which is still the only thing that tests the binding layer, and still needs the panel.

## When not to use it

A screen built once and never updated gains nothing from a reconciler, and pays 8 KB and a build step for it. [`apps/weather.js`](../app/apps/weather.js) is that shape. So is anything driving a widget imperatively at speed — pushing chart points, following a drag — where the tree is not what is changing. The imperative API is not deprecated, is not going anywhere, and remains what [`binding-api.md`](binding-api.md) documents.
