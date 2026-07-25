# JsHost architecture

How the JavaScript runtime actually works, for someone modifying it. If you only want to *write* scripts, [`binding-api.md`](binding-api.md) is the reference you want; this document is the layer beneath it. The design rationale and the alternatives that were rejected live in [`js-scripting-plan.md`](js-scripting-plan.md), and engine-level measurements and traps are in [`engine-notes.md`](engine-notes.md).

## The stack

```
        app.js                      data on SD or FATFS, never compiled
          │  loaded at boot / on reload
          ▼
    QuickJS-ng VM                   lang-js/quickjs-ng/, heap in PSRAM
          │  lv.button(p,{...}) / widget.on("click", fn)
          ▼
    binding layer                   lang-js/JsHost/js_bindings.cpp
          │  lv_button_create() / lv_obj_add_event_cb()
          ▼
       LVGL 9.5                     widget tree, layout, rendering
          │  flush callback, RGB565 little-endian
          ▼
  Arduino_GFX → JD9853 panel        lang-js/JsHost/JsHost.ino
```

Four files carry the whole design. `JsHost.ino` owns the hardware and the process lifecycle: display bring-up, LVGL wiring, the script loader, the serial protocol, and the main loop. `js_bindings.cpp` owns everything about the JavaScript world: the VM, the bindings, and the ownership machinery. `js_bindings.h` is the boundary between them, deliberately tiny (start, stop, eval a line, pump jobs, plus two callbacks the host provides). `js_fallback.h` holds the script baked into flash for when no `app.js` is found.

Note the direction of the dependency at the bottom. The bindings never reach into the sketch's globals; where they need something only the host knows, the host supplies it through `jsvm_host_fps()` and `jsvm_host_backlight()`, declared in the header and defined in the `.ino`. That inversion is what keeps `js_bindings.cpp` portable to another board: swap the `.ino` and the bindings compile unchanged.

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

The intrusive lists (`g_events`, `g_timers`) exist because the DELETE hook does not cover everything. `lv_obj_clean(screen)` deletes the screen's *children*, so a binding attached to the screen object itself never receives `LV_EVENT_DELETE`. The lists let teardown find those stragglers.

### Known hazard: dangling widget handles

Because wrappers are weak and unvalidated, a handle whose widget has been deleted still points at freed memory. Within a single script run there is exactly one way to reach that state: `.clean()` deletes children, so holding a handle to a child across a `.clean()` of its parent leaves that handle dangling, and using it afterward is undefined behavior. Across a reload the question does not arise, since teardown destroys the context holding every handle.

Current code stays clear of this. The one `.clean()` call in `app.js` is `wifiList.clean()`, and list rows are created transiently inside the scan callback, never retained. The hazard is a sharp edge waiting for a future script rather than a live bug.

The fix, if it becomes worth the cost: LVGL 9.5 provides `lv_obj_is_valid()` (`lv_obj.h:351`), so `arg_widget()` could validate before returning and throw a clean JS `TypeError` instead. That converts a memory-corruption bug into an exception, at the cost of a tree walk on every binding call.

## Teardown, and why the order is what it is

`jsvm_stop()` runs four steps, and each is where it is for a reason:

1. **Release the WiFi scan, then delete every JS timer.** Both can re-enter the VM. Killing them first guarantees no callback fires into a half-dismantled world.
2. **`lv_obj_clean(lv_screen_active())`.** This deletes the widget tree, which fires `LV_EVENT_DELETE` on every widget carrying a binding, which releases those bindings *while the context is still alive*. Freeing the context before this point would mean calling `JS_FreeValue` against a dead context.
3. **Sweep whatever remains in `g_events`.** These are bindings on the screen object itself. Their LVGL callbacks are detached explicitly with `lv_obj_remove_event_cb_with_user_data` before the struct is freed, so LVGL cannot later dispatch through a dangling `user_data`.
4. **Free the context, then the runtime, then zero the class IDs.** The IDs come from a per-runtime counter, so zeroing them makes the next `JS_NewClassID` allocate fresh rather than reuse another runtime's numbering.

Reload correctness was checked on hardware with five consecutive cycles: internal RAM was identical before and after, and PSRAM drifted 56 bytes total.

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

One script file, no module system and no `import`. No filesystem access from JS, by design: the host owns storage. Three font sizes, 14, 16 and 20, because each compiled font costs flash. No widget deletion beyond `.clean()`. No `setTimeout`. The surface is roughly fifteen functions because every addition is a permanent maintenance and correctness obligation, and the plan's stated risk was scope creep, not scarcity.
