// jsvm_core.cpp — the VM: QuickJS lifecycle, JSValue ownership, teardown order.
//
// This file owns the invariants that make the library memory-safe. The binding
// modules own vocabulary; this owns correctness.
//
//   * Every JSValue stored on the C side is JS_DupValue'd at store time and
//     freed exactly once, at a well-defined release point.
//   * Event bindings are released by an LV_EVENT_DELETE hook on their widget,
//     so deleting a widget can never leave a callback pointing at freed JS.
//   * A trampoline holds its own reference to everything it passes into JS for
//     the duration of the call: JS_Call only borrows, and a callback is allowed
//     to destroy the binding that invoked it (stop its own timer, clean away
//     its own widget). Never hand a stored JSValue straight to JS_Call.
//   * Every binding also sits in a global intrusive list; jsvm_stop() walks
//     whatever the DELETE hooks didn't reach (bindings on the screen object,
//     which lv_obj_clean() does not delete) before freeing the context.
//   * Widget wrappers hold a bare lv_obj_t* and no finalizer: LVGL owns the
//     widget tree, so a wrapper is a weak reference. jsvm_arg_widget()
//     validates every handle, so one left stale by a .clean() raises a JS
//     TypeError instead of writing into freed memory.
//
// Reasoning and the teardown-order rationale: docs/lang-js/architecture.md.

#include <esp_heap_caps.h>

#include "jsvm_internal.h"

// ---------------------------------------------------------------- VM state

static JSRuntime *g_rt = nullptr;
JSContext *jsvm_ctx = nullptr;
JSClassID jsvm_widget_class = 0;
static JSClassID g_timer_class = 0;

// How much C stack QuickJS may use before it raises a stack-overflow error.
//
// This is a BUDGET AGAINST THE CALLING TASK'S STACK, not a free choice: QuickJS
// sets stack_limit = (SP at JS_SetMaxStackSize) - this value, then fails any eval
// whose SP drops below that. Set it too close to the task's actual stack size and
// the limit lands below the real stack base, so js_check_stack_overflow() is true
// on entry and EVERY eval fails instantly with a non-stringifiable InternalError
// (seen as "[js] ERROR: null" on the very first 1+1).
//
// The host defines JS_MAX_STACK to suit its loop-task stack; 20 KB against the
// 32 KB SET_LOOP_TASK_STACK_SIZE the Waveshare target uses. Keep a healthy margin.
#ifndef JS_MAX_STACK
#define JS_MAX_STACK (20 * 1024)
#endif
static const size_t kJsMaxStack = JS_MAX_STACK;

// The JS heap comes from JS_HEAP_CAPS (jsvm_internal.h): PSRAM by default,
// byte-addressable internal DRAM on boards without it.
//
// usable_size must report 0: QuickJS treats it as writable capacity, and with
// IDF heap poisoning heap_caps_get_allocated_size() counts the tail canary in
// it — reporting real sizes corrupts the heap (proven on hardware, see
// docs/lang-js/engine-notes.md).
static void *qjs_calloc(void *, size_t n, size_t sz) { return heap_caps_calloc(n, sz, JS_HEAP_CAPS); }
static void *qjs_malloc(void *, size_t sz) { return heap_caps_malloc(sz, JS_HEAP_CAPS); }
static void qjs_free(void *, void *p) { heap_caps_free(p); }
static void *qjs_realloc(void *, void *p, size_t sz) { return heap_caps_realloc(p, sz, JS_HEAP_CAPS); }
static size_t qjs_usable_size(const void *) { return 0; }
static const JSMallocFunctions kMallocFns = {qjs_calloc, qjs_malloc, qjs_free, qjs_realloc, qjs_usable_size};

// ---------------------------------------------------------------- diagnostics

void jsvm_report_exception() {
  JSValue exc = JS_GetException(jsvm_ctx);
  const char *msg = JS_ToCString(jsvm_ctx, exc);
  Serial.printf("[js] ERROR: %s\n", msg ? msg : "(unprintable)");
  JS_FreeCString(jsvm_ctx, msg);

  // A thrown null/undefined is not a script error: QuickJS reports an internal
  // allocation failure that way, with no Error object and no stack. Say so and
  // print the heap, because "ERROR: null" alone reads like a script bug and
  // sends you looking in entirely the wrong place.
  if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
    Serial.printf("[js]   (no Error object — usually allocation failure; %u bytes free, largest block %u)\n",
                  heap_caps_get_free_size(JS_HEAP_CAPS),
                  heap_caps_get_largest_free_block(JS_HEAP_CAPS));
  }
  if (JS_IsObject(exc)) {
    JSValue stack = JS_GetPropertyStr(jsvm_ctx, exc, "stack");
    if (JS_IsString(stack)) {
      const char *s = JS_ToCString(jsvm_ctx, stack);
      if (s && *s) Serial.printf("%s", s);
      JS_FreeCString(jsvm_ctx, s);
    }
    JS_FreeValue(jsvm_ctx, stack);
  }
  JS_FreeValue(jsvm_ctx, exc);
}

void jsvm_call_reporting(JSValue fn, int argc, JSValueConst *argv) {
  JSValue r = JS_Call(jsvm_ctx, fn, JS_UNDEFINED, argc, argv);
  if (JS_IsException(r)) jsvm_report_exception();
  JS_FreeValue(jsvm_ctx, r);
}

// ---------------------------------------------------------------- app switching

// A script cannot tear down the context it is running in, so a launch request
// is parked here and collected by the host from outside the VM.
static char g_pending_launch[128];

void jsvm_request_launch(const char *name) {
  strncpy(g_pending_launch, name, sizeof(g_pending_launch) - 1);
  g_pending_launch[sizeof(g_pending_launch) - 1] = '\0';
}

const char *jsvm_take_pending_launch() {
  if (g_pending_launch[0] == '\0') return nullptr;
  // Hand back a stable copy: the host is about to call jsvm_start(), which
  // reaches jsvm_stop() and would otherwise be reading a buffer we just wiped.
  static char taken[sizeof(g_pending_launch)];
  memcpy(taken, g_pending_launch, sizeof(taken));
  g_pending_launch[0] = '\0';
  return taken;
}

// ---------------------------------------------------------------- console

static JSValue js_console_log(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  for (int i = 0; i < argc; i++) {
    const char *s = JS_ToCString(ctx, argv[i]);
    if (s) {
      if (i) Serial.print(' ');
      Serial.print(s);
      JS_FreeCString(ctx, s);
    }
  }
  Serial.println();
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------- widget handles

JSValue jsvm_wrap_widget(JSContext *ctx, lv_obj_t *obj) {
  JSValue w = JS_NewObjectClass(ctx, jsvm_widget_class);
  JS_SetOpaque(w, obj);
  return w;
}

// Writing through a stale handle used to silently succeed and corrupt the heap
// (verified on hardware), the worst failure mode available, so validate on
// every use. lv_obj_is_valid() only compares pointers while walking the screen
// tree and never dereferences the candidate, making it safe on an
// already-freed pointer, and cheap at the object counts a small UI holds.
lv_obj_t *jsvm_arg_widget(JSContext *ctx, JSValueConst v) {
  lv_obj_t *obj = static_cast<lv_obj_t *>(JS_GetOpaque2(ctx, v, jsvm_widget_class));
  if (obj == nullptr) return nullptr;  // JS_GetOpaque2 already threw
  if (!lv_obj_is_valid(obj)) {
    JS_ThrowTypeError(ctx, "widget has been deleted");
    return nullptr;
  }
  return obj;
}

// ---------------------------------------------------------------- event bindings

struct EventBinding {
  JSValue fn;      // duped
  JSValue widget;  // duped wrapper, passed as the callback's argument
  lv_obj_t *obj;
  EventBinding *prev, *next;
};
static EventBinding *g_events = nullptr;  // doubly-linked registry

static void event_unlink_and_free(EventBinding *b) {
  if (b->prev) b->prev->next = b->next; else g_events = b->next;
  if (b->next) b->next->prev = b->prev;
  JS_FreeValue(jsvm_ctx, b->fn);
  JS_FreeValue(jsvm_ctx, b->widget);
  free(b);
}

static void event_trampoline(lv_event_t *e) {
  EventBinding *b = static_cast<EventBinding *>(lv_event_get_user_data(e));
  // A handler may destroy the very widget it is attached to (any ancestor's
  // .clean(), including lv.screen().clean()), which fires LV_EVENT_DELETE and
  // frees this binding mid-call. Hold both values ourselves for the duration.
  JSValue fn = JS_DupValue(jsvm_ctx, b->fn);
  JSValue widget = JS_DupValue(jsvm_ctx, b->widget);

  // Pointer events carry the touch point: fn(widget, x, y). Non-pointer
  // dispatches (e.g. a value change set from code) just get fn(widget).
  lv_indev_t *indev = lv_indev_active();
  if (indev) {
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    JSValue args[3] = {widget, JS_NewInt32(jsvm_ctx, pt.x), JS_NewInt32(jsvm_ctx, pt.y)};
    jsvm_call_reporting(fn, 3, args);
    JS_FreeValue(jsvm_ctx, args[1]);
    JS_FreeValue(jsvm_ctx, args[2]);
  } else {
    jsvm_call_reporting(fn, 1, &widget);
  }

  JS_FreeValue(jsvm_ctx, fn);
  JS_FreeValue(jsvm_ctx, widget);
}

static void event_delete_cb(lv_event_t *e) {
  event_unlink_and_free(static_cast<EventBinding *>(lv_event_get_user_data(e)));
}

bool jsvm_bind_event(JSContext *ctx, lv_obj_t *obj, lv_event_code_t code,
                     JSValueConst fn, JSValueConst widget) {
  EventBinding *b = static_cast<EventBinding *>(malloc(sizeof(EventBinding)));
  if (!b) return false;
  b->fn = JS_DupValue(ctx, fn);
  b->widget = JS_DupValue(ctx, widget);
  b->obj = obj;
  b->prev = nullptr;
  b->next = g_events;
  if (g_events) g_events->prev = b;
  g_events = b;

  lv_obj_add_event_cb(obj, event_trampoline, code, b);
  lv_obj_add_event_cb(obj, event_delete_cb, LV_EVENT_DELETE, b);
  return true;
}

// ---------------------------------------------------------------- timer bindings

struct TimerBinding {
  JSValue fn;    // duped
  JSValue self;  // duped timer wrapper — keeps opaque valid while stored
  lv_timer_t *t;
  TimerBinding *prev, *next;
};
static TimerBinding *g_timers = nullptr;

static void timer_release(TimerBinding *b, bool delete_lv_timer) {
  if (b->prev) b->prev->next = b->next; else g_timers = b->next;
  if (b->next) b->next->prev = b->prev;
  if (delete_lv_timer) lv_timer_delete(b->t);
  JS_SetOpaque(b->self, nullptr);  // make a later .stop() a no-op
  JS_FreeValue(jsvm_ctx, b->fn);
  JS_FreeValue(jsvm_ctx, b->self);
  free(b);
}

static void timer_trampoline(lv_timer_t *t) {
  TimerBinding *b = static_cast<TimerBinding *>(lv_timer_get_user_data(t));
  // Hold our own reference across the call. The callback may stop its own
  // timer — `const t = lv.timer(ms, () => t.stop())` is the one-shot idiom —
  // which frees this binding and its JSValues. JS_Call only *borrows*
  // func_obj, so dropping the last reference mid-call frees the closure
  // underneath the running interpreter (verified: LoadProhibited panic).
  JSValue fn = JS_DupValue(jsvm_ctx, b->fn);
  jsvm_call_reporting(fn, 0, nullptr);
  JS_FreeValue(jsvm_ctx, fn);
}

static JSValue js_timer_stop(JSContext *ctx, JSValueConst this_val, int, JSValueConst *) {
  TimerBinding *b = static_cast<TimerBinding *>(JS_GetOpaque(this_val, g_timer_class));
  if (b) timer_release(b, true);
  return JS_UNDEFINED;
}

JSValue jsvm_create_timer(JSContext *ctx, int32_t ms, JSValueConst fn) {
  if (ms < 10) ms = 10;  // floor: a 0ms JS timer would starve rendering

  TimerBinding *b = static_cast<TimerBinding *>(malloc(sizeof(TimerBinding)));
  if (!b) return JS_ThrowOutOfMemory(ctx);
  JSValue self = JS_NewObjectClass(ctx, g_timer_class);
  JS_SetOpaque(self, b);
  b->fn = JS_DupValue(ctx, fn);
  b->self = JS_DupValue(ctx, self);
  b->t = lv_timer_create(timer_trampoline, ms, b);
  b->prev = nullptr;
  b->next = g_timers;
  if (g_timers) g_timers->prev = b;
  g_timers = b;
  return self;
}

// ---------------------------------------------------------------- setup / teardown

static const JSClassDef kWidgetClassDef = { "LvWidget", nullptr, nullptr, nullptr, nullptr };
static const JSClassDef kTimerClassDef = { "LvTimer", nullptr, nullptr, nullptr, nullptr };

// Builds the JS context. JS_NewContext() loads twelve intrinsic groups whether a
// script wants them or not, and on a board without PSRAM that startup cost is
// most of the available heap. JS_LEAN_CONTEXT builds the context from
// JS_NewContextRaw() plus only the groups the shipped scripts actually use.
//
// What the scripts in app/ use, verified by grep before choosing this list:
//   BaseObjects  Object/Array/String/Error — not optional
//   Eval         JS_Eval itself needs it
//   JSON         weather.js parses its API response and its disk cache
//   Promise      vitals.js, weather.js, selftest.js use async/await
//   MapSet       wifi.js keeps a Set of seen SSIDs
//   RegExp       not used by any app, but String.prototype.split/replace with a
//                string arg still routes through it internally, so it stays
//
// Dropped: Date, Proxy, TypedArrays, WeakRef, AToB, Performance. If a script
// needs one later, add it here rather than reaching for JS_NewContext.
static JSContext *new_context(JSRuntime *rt) {
#if JS_LEAN_CONTEXT
  JSContext *ctx = JS_NewContextRaw(rt);
  if (!ctx) return nullptr;
  if (JS_AddIntrinsicBaseObjects(ctx) || JS_AddIntrinsicEval(ctx) ||
      JS_AddIntrinsicRegExp(ctx) || JS_AddIntrinsicJSON(ctx) ||
      JS_AddIntrinsicMapSet(ctx) || JS_AddIntrinsicPromise(ctx)) {
    JS_FreeContext(ctx);
    return nullptr;
  }
  return ctx;
#else
  return JS_NewContext(rt);
#endif
}

static void install_core_globals(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);

  // Timer prototype: the class is core's, since core owns the binding it wraps.
  JSValue tproto = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, tproto, "stop", JS_NewCFunction(ctx, js_timer_stop, "stop", 0));
  JS_SetClassProto(ctx, g_timer_class, tproto);

  // console is core, not part of the optional sys module: a script that cannot
  // print has no way to report anything.
  JSValue console = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
  JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_log, "error", 1));
  JS_SetPropertyStr(ctx, global, "console", console);

  JS_FreeValue(ctx, global);
}

bool jsvm_running() { return jsvm_ctx != nullptr; }

bool jsvm_start(const char *src, const char *filename) {
  if (g_rt) jsvm_stop();

  // Report the pool the VM actually draws from, since that is what determines
  // whether startup can succeed. A bare "failed" here is very hard to act on:
  // on a PSRAM-less board the usual cause is simply not enough free memory
  // after LVGL and the WiFi stack have taken theirs.
  const uint32_t heap_before = heap_caps_get_free_size(JS_HEAP_CAPS);

  g_rt = JS_NewRuntime2(&kMallocFns, nullptr);
  if (!g_rt) {
    Serial.printf("[js] runtime creation failed (%u bytes free, largest block %u)\n",
                  heap_before, heap_caps_get_largest_free_block(JS_HEAP_CAPS));
    return false;
  }
  JS_SetMaxStackSize(g_rt, kJsMaxStack);
  jsvm_ctx = new_context(g_rt);
  if (!jsvm_ctx) {
    Serial.printf("[js] context creation failed (%u bytes free at start, %u now, largest block %u)\n",
                  heap_before, heap_caps_get_free_size(JS_HEAP_CAPS),
                  heap_caps_get_largest_free_block(JS_HEAP_CAPS));
    JS_FreeRuntime(g_rt);
    g_rt = nullptr;
    return false;
  }

  JS_NewClassID(g_rt, &jsvm_widget_class);
  JS_NewClass(g_rt, jsvm_widget_class, &kWidgetClassDef);
  JS_NewClassID(g_rt, &g_timer_class);
  JS_NewClass(g_rt, g_timer_class, &kTimerClassDef);

  // Composition root: the one place that names the modules.
  install_core_globals(jsvm_ctx);
  js_install_lv(jsvm_ctx);
#if JSVM_WITH_SYS
  js_install_sys(jsvm_ctx);
#endif
#if JSVM_WITH_FS
  js_install_fs(jsvm_ctx);
#endif
#if JSVM_WITH_WIFI
  js_install_wifi(jsvm_ctx);
#endif

  // What the VM cost to stand up, and what a script has left to work with. On a
  // PSRAM-less board this is the difference between "runs" and "runs anything
  // useful", so it is worth a line every boot.
  Serial.printf("[js] vm ready: %u bytes to start, %u free for scripts\n",
                heap_before - heap_caps_get_free_size(JS_HEAP_CAPS),
                heap_caps_get_free_size(JS_HEAP_CAPS));

  const uint32_t t0 = millis();
  JSValue r = JS_Eval(jsvm_ctx, src, strlen(src), filename, JS_EVAL_TYPE_GLOBAL);
  const bool ok = !JS_IsException(r);
  if (!ok) jsvm_report_exception();
  JS_FreeValue(jsvm_ctx, r);
  Serial.printf("[js] %s: eval %s in %lu ms\n", filename, ok ? "ok" : "FAILED",
                (unsigned long)(millis() - t0));
  return ok;
}

void jsvm_repl_line(const char *src) {
  if (!jsvm_ctx) {
    Serial.println("[repl] no VM running");
    return;
  }
  JSValue r = JS_Eval(jsvm_ctx, src, strlen(src), "<repl>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) {
    jsvm_report_exception();
  } else {
    const char *s = JS_ToCString(jsvm_ctx, r);
    Serial.printf("[repl] %s\n", s ? s : "(unprintable)");
    JS_FreeCString(jsvm_ctx, s);
  }
  JS_FreeValue(jsvm_ctx, r);
}

void jsvm_pump() {
  if (!g_rt) return;
  JSContext *jctx;
  int r;
  while ((r = JS_ExecutePendingJob(g_rt, &jctx)) > 0) {}
  if (r < 0) jsvm_report_exception();
}

void jsvm_stop() {
  if (!g_rt) return;

  // Order matters. Anything that can re-enter the VM dies first, so no callback
  // can fire into a half-dismantled world: module state holding callbacks, then
  // JS-driven lv_timers.
#if JSVM_WITH_WIFI
  js_teardown_wifi();
#endif
  while (g_timers) timer_release(g_timers, true);

  // Deleting the widget tree fires LV_EVENT_DELETE, releasing per-widget
  // bindings while the context is still alive.
  lv_obj_clean(lv_screen_active());

  // Whatever remains was bound to the screen object itself, which
  // lv_obj_clean() does not delete. Detach and release it explicitly.
  while (g_events) {
    EventBinding *b = g_events;
    lv_obj_remove_event_cb_with_user_data(b->obj, event_trampoline, b);
    lv_obj_remove_event_cb_with_user_data(b->obj, event_delete_cb, b);
    event_unlink_and_free(b);
  }

  JS_FreeContext(jsvm_ctx);
  JS_FreeRuntime(g_rt);
  jsvm_ctx = nullptr;
  g_rt = nullptr;
  // Class IDs come from a per-runtime counter; zero them so the next runtime
  // allocates fresh ones instead of reusing another runtime's numbering.
  jsvm_widget_class = 0;
  g_timer_class = 0;
}
