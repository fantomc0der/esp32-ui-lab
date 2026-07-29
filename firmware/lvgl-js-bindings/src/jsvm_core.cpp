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
// Reasoning and the teardown-order rationale: docs/runtime-architecture.md.

#include <esp_heap_caps.h>

#include "jsvm_internal.h"

// ---------------------------------------------------------------- VM state

static JSRuntime *g_rt = nullptr;
JSContext *jsvm_ctx = nullptr;
JSClassID jsvm_widget_class = 0;
static JSClassID g_timer_class = 0;

static const size_t kJsMaxStack = 20 * 1024;  // loopTask stack is 32 KB

// JS heap in PSRAM. usable_size must report 0: QuickJS treats it as writable
// capacity, and with IDF heap poisoning heap_caps_get_allocated_size() counts
// the tail canary in it — reporting real sizes corrupts the heap (proven on
// hardware, see docs/engine-notes.md).
static void *qjs_calloc(void *, size_t n, size_t sz) { return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM); }
static void *qjs_malloc(void *, size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
static void qjs_free(void *, void *p) { heap_caps_free(p); }
static void *qjs_realloc(void *, void *p, size_t sz) { return heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM); }
static size_t qjs_usable_size(const void *) { return 0; }
static const JSMallocFunctions kMallocFns = {qjs_calloc, qjs_malloc, qjs_free, qjs_realloc, qjs_usable_size};

// ---------------------------------------------------------------- diagnostics

void jsvm_report_exception() {
  JSValue exc = JS_GetException(jsvm_ctx);
  const char *msg = JS_ToCString(jsvm_ctx, exc);
  Serial.printf("[js] ERROR: %s\n", msg ? msg : "(unprintable)");
  JS_FreeCString(jsvm_ctx, msg);

  // A thrown null or undefined carries no stack, so the line above is the whole
  // message. Usually that is a script's own `throw null` or a promise rejected
  // with no argument, but it is also how a failure deep enough that the engine
  // could not allocate the Error object to describe it comes out. The heap
  // reading is what tells those apart.
  if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
    Serial.printf("[js]   (no Error object — a bare throw, or an allocation failure; %u bytes free, largest block %u)\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
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

  // Report the pool the VM draws from, since that is what decides whether
  // startup can succeed at all. A bare "failed" here is very hard to act on.
  const uint32_t heap_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  g_rt = JS_NewRuntime2(&kMallocFns, nullptr);
  if (!g_rt) {
    Serial.printf("[js] runtime creation failed (%u bytes free, largest block %u)\n",
                  heap_before, heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return false;
  }
  JS_SetMaxStackSize(g_rt, kJsMaxStack);
  jsvm_ctx = JS_NewContext(g_rt);
  if (!jsvm_ctx) {
    Serial.printf("[js] context creation failed (%u bytes free at start, %u now, largest block %u)\n",
                  heap_before, heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
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

  // What the VM cost to stand up, and what a script has left to work with. The
  // second number is the one that decides whether a given script can evaluate,
  // since source text, bytecode and the resulting object graph all coexist.
  Serial.printf("[js] vm ready: %u bytes to start, %u free for scripts\n",
                heap_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);

  // The screen object itself survives that, and lv.screen().set(...) writes
  // local styles and flags straight onto it — a background colour, padding, a
  // disabled scroll. Left in place they would show through the next app
  // wherever it does not set the same thing. Strip them back to what a freshly
  // created screen has: theme styles, and the flags lv_obj's constructor gives
  // a parentless object.
  lv_obj_remove_style_all(screen);
  lv_theme_apply(screen);
  lv_obj_add_flag(screen, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_HIDDEN);

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
