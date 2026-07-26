# js-host architecture

How the JavaScript runtime actually works, for someone modifying it. If you only want to *write* scripts, [`binding-api.md`](binding-api.md) is the reference you want; this document is the layer beneath it. The design rationale and the alternatives that were rejected live in [`design-rationale.md`](design-rationale.md), and engine-level measurements and traps are in [`engine-notes.md`](engine-notes.md).

## The stack

```
        app.js                      data on SD or FATFS, never compiled
          │  loaded at boot / on reload
          ▼
    QuickJS-ng VM                   lang-js/quickjs-ng/, heap in PSRAM
          │  lv.button(p,{...}) / widget.on("click", fn)
          ▼
    binding layer                   lang-js/lvgl-js-bindings/
          │  lv_button_create() / lv_obj_add_event_cb()
          ▼
       LVGL 9.5                     widget tree, layout, rendering
          │  flush callback, RGB565 little-endian
          ▼
  Arduino_GFX → JD9853 panel        lang-js/js-host/js-host.ino
```

The split between the sketch and the binding library is a deliberate boundary rather than filing. Two Arduino libraries sit beside the sketch and are linked with `--library`: `quickjs-ng/` (the vendored engine) and `lvgl-js-bindings/` (the bindings). Neither knows anything about this board.

Inside the binding library, one file owns correctness and the rest own vocabulary:

| File | Owns |
|---|---|
| `js_bindings.h` | the entire public surface: start, stop, eval a line, pump jobs, plus the three host hooks |
| `jsvm_internal.h` | what the core shares with the modules, and nothing more |
| `jsvm_core.cpp` | QuickJS lifecycle, the PSRAM allocator, JSValue ownership, the trampolines, teardown order, `console` |
| `bindings_lv.cpp` | the `lv` global: widget constructors, props, widget methods |
| `bindings_sys.cpp` | the `sys` global |
| `bindings_wifi.cpp` | the `wifi` global |

The rule that keeps the split honest: **a module never stores a `JSValue`.** Anything that must outlive a call is handed to the core through `jsvm_bind_event()` or `jsvm_create_timer()`, which dup it and free it at one place. That is why the ownership rules can be reasoned about by reading a single file, even though four files can hold callbacks.

`jsvm_core.cpp` is also the composition root, so it is the one place naming the modules: it calls each `js_install_*()` on every start, since a reload builds a fresh context. Modules holding state across calls also expose a teardown, which the core runs first, before any widget is deleted. Today only `wifi` needs one. `sys` and `wifi` can be compiled out entirely with `-DJSVM_WITH_SYS=0` / `-DJSVM_WITH_WIFI=0`, which is what makes the library usable on a board with no radio without editing it.

Per-widget files were considered and rejected. `lv_binding_js` needs them because a React reconciler wants per-component prop diffing; here all nine widgets share one `apply_props`, so a widget is an enum value, a `switch` case, and a table row. Nine files of ten lines would be more structure describing less code.

In the sketch, `js-host.ino` owns the hardware and the process lifecycle: display bring-up, LVGL wiring, the script loader, the serial protocol, and the main loop. `js_fallback.h` holds the script baked into flash for when no `app.js` is found. The hardware headers (`board_pins.h`, `jd9853_panel.h`, `axs5106l_touch.*`, `lv_conf.h`) are verbatim copies from the C demo.

Note the direction of the dependency. The bindings never reach into the sketch's globals; where they need something only the host knows, the host supplies it through `jsvm_host_fps()`, `jsvm_host_backlight()`, and `jsvm_host_battery()`, declared in the library header and defined in the `.ino`. Those three functions are the entire porting contract: bring LVGL up however a different board requires, implement them, and the binding library compiles unchanged.

One build subtlety worth knowing: the sketch folder is on the include path for library compilation units (verified in the build's `-I` flags), which is how the sketch-local `lv_conf.h` governs LVGL *and* the binding library. Both see the same LVGL configuration, so there is no risk of the two disagreeing about struct layouts.

## The threading rule

**Everything JavaScript-related runs on `loopTask`, the same FreeRTOS task that calls `lv_timer_handler()`. Nothing else may touch the VM or LVGL.**

This single rule is what the entire design rests on, and it buys a great deal: no mutexes anywhere, no LVGL locking, no atomics, no concurrent GC hazards. A JS event callback cannot interleave with rendering because rendering is a function call on the same stack. It also means the rule is load-bearing rather than incidental: violating it does not produce a clean crash, it produces intermittent memory corruption.

Three places the rule shows up concretely. Timers are `lv_timer_create`, never FreeRTOS timers, so their callbacks are dispatched from inside `lv_timer_handler()`. The WiFi scan is asynchronous in the radio driver but its *completion* is detected by polling from an `lv_timer`, so the JS callback still runs on `loopTask`. Promise reactions are drained by `jsvm_pump()` at the bottom of `loop()` rather than from any callback context.

The rule is also the main constraint on future work. Every asynchronous network library on ESP32 (ESPAsyncWebServer, most MQTT clients) invokes its callbacks on its own task. Calling `jsvm_repl_line()`, `jsvm_start()`, or any `lv_*` function from such a callback breaks the invariant. The pattern that stays safe: the network callback copies its payload into a queue and returns, and `loop()` drains that queue and does the real work. A synchronous server polled from `loop()` sidesteps the problem entirely.

## Boot and lifecycle

```
setup()
  backlight off ─ panel reset ─ gfx->begin ─ jd9853_init ─ rotation
  Wire + touch_begin
  WiFi.mode(STA)                     radio up, not connected (scan needs this)
  lv_init, tick cb, draw buffers, display, pointer indev
  runApp() ─────────────► first script starts here
  backlight on

loop()  (forever)
  lv_timer_handler()                 renders, fires lv_timers → JS callbacks
  fps accounting                     flush_count over a 1 s window
  BOOT long-press check              ≥ 700 ms → runApp()
  pollSerialRepl()                   host commands, or eval one line
  jsvm_pump()                        drain promise jobs
  delay(idle_ms)                     LVGL's own sleep hint, capped at 16 ms
```

`runApp()` is the only entry point that starts a script, and it is deliberately re-runnable at any time. It calls `jsvm_stop()` first (safe when nothing is running), searches storage in a fixed order, then starts the VM:

1. `sd:/app.js`, with the card mounted fresh on every attempt so a card swapped while powered is seen, then unmounted immediately.
2. `ffat:/app.js` on the 9.9 MB FATFS partition, formatting it on first use.
3. `kFallbackScript` compiled into flash.

The script source is read into a PSRAM buffer, evaluated, and freed; QuickJS copies what it needs during compilation. A script that throws during its top-level evaluation is reported with its stack, torn down, and replaced by the fallback screen, so a syntax error in `app.js` cannot leave the board dark or wedged.

## Anatomy of a call, JavaScript into C

Take `lv.button(parent, { text: "Scan", w: 84 })`.

All nine constructors are the same C function. `js_lv_make` is registered once per widget type with `JS_NewCFunctionMagic`, and QuickJS passes back the `magic` integer it was registered with, which is the `WidgetKind` enum value. That is why adding a widget type is a two-line change rather than a new function.

The call unwraps its parent with `arg_widget()`, which is `JS_GetOpaque2` against the widget class ID: it both fetches the `lv_obj_t*` and type-checks the JS object, throwing if a script passes something that is not a widget. It creates the LVGL object, applies props, and wraps the result.

`apply_props()` is a flat sequence of "read key, act if present, free the value". Two conventions matter there. Unknown keys are ignored rather than rejected, so a script written for newer firmware degrades on older firmware instead of throwing. And every `JSValue` returned by `JS_GetPropertyStr` is freed on every path, including the ones where the value is not used, which is why the function reads repetitively; that repetition is the leak-avoidance discipline, not an accident.

Wrapping is `wrap_widget()`: a new object of the widget class with the `lv_obj_t*` stored as its opaque pointer. The class has **no finalizer**, deliberately, which is the subject of the ownership section below.

## Anatomy of an event, C into JavaScript

```
finger on glass
  └─ lv_touch_cb                 (indev read cb, polls AXS5106L over I2C)
      └─ LVGL hit-tests, sends LV_EVENT_CLICKED to the widget
          └─ event_trampoline    (user_data = EventBinding*)
              ├─ lv_indev_active() non-null → read the point
              └─ JS_Call(binding->fn, [widget, x, y])
                  └─ script's handler runs
```

`.on(name, fn)` allocates an `EventBinding` holding a duped reference to the function and to the widget wrapper, links it into a global list, and registers **two** LVGL callbacks against it: `event_trampoline` for the requested event, and `event_delete_cb` for `LV_EVENT_DELETE`. The second one is the ownership mechanism, described next.

Arguments are shaped by whether a pointer drove the event. When `lv_indev_active()` returns a device, the handler receives `(widget, x, y)` in screen coordinates, which is what the Touch tab uses to put a dot under the finger, combined with `.bounds()` to convert into a container's content area. When the event came from code (a `.value(n)` call raising `LV_EVENT_VALUE_CHANGED`), the handler receives only `(widget)`.

Exceptions never propagate into C. `js_call_reporting()` prints the message and stack to serial and continues, so a broken handler produces a stream of complaints rather than a dead board. This was verified on hardware by arming a timer whose callback always throws.

## Handles and ownership

This is the part to understand before changing anything, because it is where a mistake costs memory corruption rather than a compile error.

**Every JSValue stored on the C side is duplicated once at store time and released at exactly one well-defined point.**

| Stored value | Duped at | Released at |
|---|---|---|
| `EventBinding.fn`, `.widget` | `.on()` | the widget's `LV_EVENT_DELETE` hook, or the `jsvm_stop()` sweep |
| `TimerBinding.fn`, `.self` | `lv.timer()` | `.stop()`, or `jsvm_stop()` |
| `g_wifi.cb` | `wifi.scan()` | scan completion, released *before* the callback runs, or `jsvm_stop()` |

Two QuickJS classes exist. `LvWidget` stores an `lv_obj_t*` and `LvTimer` stores a `TimerBinding*`. Neither has a finalizer, for different reasons.

For widgets, LVGL owns the tree; the JS object is a **weak reference**. Wrappers are cheap and duplicable (`lv.screen()` called twice yields two objects pointing at one widget), so a finalizer that deleted the widget would be wrong.

For timers, the opaque is nulled inside `timer_release()` before the struct is freed, which makes a later `.stop()` on a stopped timer a harmless no-op instead of a double free.

### Re-entrancy: trampolines must hold their own references

A callback is allowed to destroy the binding that invoked it. `lv.timer(ms, () => t.stop())` is the ordinary one-shot idiom, and a click handler calling `lv.screen().clean()` deletes the very widget being dispatched. Both free the binding, and with it the `JSValue`s the trampoline is mid-call on.

`JS_Call` **borrows** its function and arguments; it neither consumes nor retains a reference (that is what the separate `JS_CallFree` is for). So releasing the last reference to a function while it is executing frees the closure underneath the interpreter. On hardware this presents as a `LoadProhibited` panic, not a clean error.

Every trampoline therefore dups what it passes in, calls, then frees. `wifi_poll_timer()` shows the same shape for a different reason: it releases the scan slot before invoking the callback, so the callback may start a new scan, and holds the function alive across the call by hand.

The intrusive lists (`g_events`, `g_timers`) exist because the DELETE hook does not cover everything. `lv_obj_clean(screen)` deletes the screen's *children*, so a binding attached to the screen object itself never receives `LV_EVENT_DELETE`. The lists let teardown find those stragglers.

### Stale widget handles

Because wrappers are weak, a handle whose widget has been deleted points at freed memory. `.clean()` deletes children, so holding a handle to a child across a `.clean()` of its container reaches that state within a single script run. (Across a reload the question does not arise, since teardown destroys the context holding every handle.)

Left unchecked this had the worst failure mode available: writing through a stale handle **silently succeeded**, corrupting the heap with no crash and no error, confirmed on hardware. So `arg_widget()`, which every binding call goes through to unwrap its subject, now validates with `lv_obj_is_valid()` and throws a JS `TypeError: widget has been deleted` instead. Scripts can catch that; freed memory they cannot.

The check is affordable because of what `lv_obj_is_valid()` does: it walks the screen tree comparing pointers and never dereferences the candidate, which is both why it is safe on an already-freed pointer and why it costs only a few hundred comparisons on a UI this size. Measured frame rate is unchanged.

## Teardown, and why the order is what it is

`jsvm_stop()` runs four steps, and each is where it is for a reason:

1. **Release the WiFi scan, then delete every JS timer.** Both can re-enter the VM. Killing them first guarantees no callback fires into a half-dismantled world.
2. **`lv_obj_clean(lv_screen_active())`.** This deletes the widget tree, which fires `LV_EVENT_DELETE` on every widget carrying a binding, which releases those bindings *while the context is still alive*. Freeing the context before this point would mean calling `JS_FreeValue` against a dead context.
3. **Sweep whatever remains in `g_events`.** These are bindings on the screen object itself. Their LVGL callbacks are detached explicitly with `lv_obj_remove_event_cb_with_user_data` before the struct is freed, so LVGL cannot later dispatch through a dangling `user_data`.
4. **Free the context, then the runtime, then zero the class IDs.** The IDs come from a per-runtime counter, so zeroing them makes the next `JS_NewClassID` allocate fresh rather than reuse another runtime's numbering.

Reload correctness was checked on hardware with five consecutive cycles: internal RAM was identical before and after, and PSRAM drifted 56 bytes total.

## Switching apps

A script cannot switch apps synchronously. `sys.launch()` would have to call `jsvm_stop()`, which destroys the `JSContext` while the calling function is still executing inside it — the same use-after-free class as the trampoline hazard above. So it only records a name. The host collects it with `jsvm_take_pending_launch()` at the end of `loop()`, after LVGL dispatch, the serial REPL, and the promise queue have all unwound, and performs the switch there. The corner button and the BOOT long-press feed the same queue rather than acting directly, for the same reason.

The way back is deliberately not the app's responsibility. The firmware creates a button on `lv_layer_top()`, which is not a child of the active screen, so `jsvm_stop()`'s `lv_obj_clean(lv_screen_active())` cannot delete it and no script can reach it to break it. An app that draws over its whole screen, or throws while building, still leaves you a way out — and BOOT works even if touch has stopped responding.

## The corner button

The bottom-right corner is one slot holding at most one control, and `updateCornerButton()` decides which, from three inputs: what is running, whether a pin is set, and whether the running app wants a network the board has not got. It runs once per `loop()` and touches LVGL only when the answer changes, so nothing that causes a change has to remember to announce it.

| State | Shown when | Goes to |
| --- | --- | --- |
| Wi-Fi | `jsvm_network_setup_needed()`, and the Wi-Fi app is not already what's running | `kWifiApp` |
| Back | a pin is set and something other than the pinned app is running | the pinned app |
| Home | no pin, and something other than the launcher is running | `kLauncher` |
| nothing | you are already where the button would take you | — |

Setup outranks navigation because it is the more actionable of the two and the harder one to reach by other means: a pinned board draws no back button while its app is running, so without the Wi-Fi state that corner is empty and the only route to network setup is a BOOT long-press nobody discovers. Ranking it above Home costs an unpinned user one extra tap through the launcher, which is a fair trade for a rule that reads the same on both kinds of board.

## Pinning one app

A pin is a single NVS string, written by `sys.pin()` or the host's `pin` command and read back by `jsvm_pinned_app()`. The binding library only remembers the preference; what to do about it is host policy, which keeps the split intact — the library still knows nothing about launchers or corner buttons. `js-host` reads it in two places: `setup()` boots the pinned script instead of `kLauncher`, and `updateCornerButton()` treats a pin as "the launcher is not part of this product", which both hides the button while the pinned app is running and redefines the way back as the pinned app rather than the launcher. The value it reads is cached in RAM, so polling it every `loop()` does not hit NVS.

Two properties are worth preserving if this changes. A pinned script that fails to load still falls back to the launcher, so a bad pin cannot brick the panel. And the BOOT long-press deliberately ignores the pin: it is the only route back to the launcher once the corner button no longer offers one, and therefore the only way to unpin a board with nothing plugged into it.

## Knowing an app wants the network

`jsvm_network_setup_needed()` is true when the running script has called `fetch()` or `wifi.status()` **and** nothing is saved **and** nothing is connected. It is what the corner button's Wi-Fi state is gated on, and it exists so that a board pinned to a network app is not a dead end when no network was ever configured.

Interest is inferred from use rather than declared through an API such as `sys.needsWifi()`. A declaration is one more thing every app author has to remember, and forgetting it fails in exactly the confusing way the button is there to prevent; using the network, on the other hand, is not something an app that needs it can omit, nor something an app that doesn't need it does by accident. The cost is timing — the flag is set on the first call, not at load — which is invisible in practice because a network app asks about the radio in its opening lines.

`wifi.status()` counts alongside `fetch()` because the polite form of a network app checks before it fetches (`weather.js` does), so keying on a thrown `fetch()` alone would miss the apps that behave best. The flag is per-script: `js_teardown_wifi()` clears it, so interest never outlives the app that showed it.

The other two conditions are about the device, and both directions matter. Credentials that are saved but failing suppress the button however badly the link is behaving, because reconnection is already supervised and a setup screen has nothing to fix — the app should say "offline" instead. An active connection suppresses it too, which covers a board that joined by some route other than `wifi.save()`.

## Asynchronous work

There is no event loop in the usual sense. Two mechanisms cover what scripts need.

**Native async** follows the `wifi.scan` pattern: start the operation, create an `lv_timer` that polls for completion, and invoke the stored JS callback from that timer. Note the ordering inside `wifi_poll_timer()`: scan state is released *before* the callback is invoked (with the function reference held alive across the call), so the callback is free to start another scan immediately without tripping the "already scanning" guard.

**Promises** are QuickJS's own job queue. `JS_ExecutePendingJob` normally belongs to quickjs-libc's event loop, which is not vendored here, so `jsvm_pump()` drains it once per `loop()`. Without that pump `.then()` and `await` continuations queue forever while everything else appears to work, which is a memorably confusing failure mode. There is no `setTimeout`; `lv.timer` fills that role.

## Where memory goes

| Region | Contents |
|---|---|
| Flash (3 MB app partition) | firmware, 1.79 MB, of which the QuickJS engine is ~429 KB |
| Flash (9.9 MB FATFS) | `app.js` when deployed to internal storage |
| Internal SRAM | two LVGL draw buffers, 13,440 bytes each, which **must** be internal and DMA-capable because SPI DMA cannot reach PSRAM; the LVGL object tree and styles; the WiFi stack; ~350 bytes of QuickJS bookkeeping |
| PSRAM | the entire JS heap, ~80 KB at rest, via the custom `JSMallocFunctions`; the `app.js` source buffer while loading |
| `loopTask` stack | 32 KB via `SET_LOOP_TASK_STACK_SIZE`, with QuickJS limited to 20 KB by `JS_SetMaxStackSize` so the interpreter hits its own guard before smashing the task stack |

The allocator has one non-negotiable rule, learned by crashing: `js_malloc_usable_size` must report 0. See [`engine-notes.md`](engine-notes.md).

## Extending it

**A new prop:** add a block to `apply_props()`. Read the key, act if present, free the value on every path. Type-specific props go inside an `lv_obj_check_type()` guard, as the arc and chart props do.

**A new widget:** add an enum value to `WidgetKind`, a `case` in `js_lv_make`'s switch, and an entry in the `kMakers` table. If it needs post-creation setup, follow the chart precedent, which stashes its series pointer in the widget's `user_data` so `.push()` can find it later.

**A new event:** add a row to `kEvents`. The trampoline and the DELETE-hook ownership come for free.

**A new native call:** write a `JSValue fn(JSContext*, JSValueConst, int argc, JSValueConst*)`, check `argc` before touching `argv`, and register it in `install_globals()`. If it stores a JS callback, add a row to the ownership table above and make sure `jsvm_stop()` releases it.

**Anything involving another task:** re-read the threading rule first. Queue the payload, do the work in `loop()`.

## Deliberate limitations

One script file, no module system and no `import`. No filesystem access from JS, by design: the host owns storage. Three font sizes, 14, 16 and 20, because each compiled font costs flash. No widget deletion beyond `.clean()`. No `setTimeout`. The surface is about twenty functions because every addition is a permanent maintenance and correctness obligation, and the [stated risk](design-rationale.md) was scope creep, not scarcity.
