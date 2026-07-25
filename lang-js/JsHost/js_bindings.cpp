// js_bindings.cpp — hand-written glue between QuickJS-ng and LVGL 9.
//
// Ownership rules (the correctness core of this whole project, see the plan):
//   * Every JSValue stored on the C side (event callbacks, timer callbacks,
//     widget wrappers passed back into callbacks) is JS_DupValue'd at store
//     time and freed exactly once, at a well-defined release point.
//   * Event bindings are released by an LV_EVENT_DELETE hook on their widget,
//     so deleting a widget can never leave a callback pointing at freed JS.
//   * Every binding also sits in a global intrusive list; jsvm_stop() walks
//     whatever the DELETE hooks didn't reach (bindings on the screen object,
//     which lv_obj_clean() does not delete) before freeing the context.
//   * Widget wrappers hold a bare lv_obj_t* and no finalizer: LVGL owns the
//     widget tree. In v1 scripts cannot delete widgets, so a wrapper can only
//     dangle across a reload — and a reload destroys the context holding it.

#include "js_bindings.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "board_pins.h"
#include "quickjs.h"

// ---------------------------------------------------------------- VM state

static JSRuntime *g_rt = nullptr;
static JSContext *g_ctx = nullptr;
static JSClassID g_widget_class = 0;
static JSClassID g_timer_class = 0;

static const size_t kJsMaxStack = 20 * 1024;  // loopTask stack is 32 KB

// JS heap in PSRAM. usable_size must report 0: QuickJS treats it as writable
// capacity, and with IDF heap poisoning heap_caps_get_allocated_size() counts
// the tail canary in it — reporting real sizes corrupts the heap (proven on
// hardware, see docs/lang-js/engine-notes.md).
static void *qjs_calloc(void *, size_t n, size_t sz) { return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM); }
static void *qjs_malloc(void *, size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
static void qjs_free(void *, void *p) { heap_caps_free(p); }
static void *qjs_realloc(void *, void *p, size_t sz) { return heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM); }
static size_t qjs_usable_size(const void *) { return 0; }
static const JSMallocFunctions kMallocFns = {qjs_calloc, qjs_malloc, qjs_free, qjs_realloc, qjs_usable_size};

// ---------------------------------------------------------------- diagnostics

static void js_report_exception() {
  JSValue exc = JS_GetException(g_ctx);
  const char *msg = JS_ToCString(g_ctx, exc);
  Serial.printf("[js] ERROR: %s\n", msg ? msg : "(unprintable)");
  JS_FreeCString(g_ctx, msg);
  if (JS_IsObject(exc)) {
    JSValue stack = JS_GetPropertyStr(g_ctx, exc, "stack");
    if (JS_IsString(stack)) {
      const char *s = JS_ToCString(g_ctx, stack);
      if (s && *s) Serial.printf("%s", s);
      JS_FreeCString(g_ctx, s);
    }
    JS_FreeValue(g_ctx, stack);
  }
  JS_FreeValue(g_ctx, exc);
}

// Call a stored JS callback and swallow-but-report exceptions, so a buggy
// script handler can't take down the firmware loop.
static void js_call_reporting(JSValue fn, int argc, JSValueConst *argv) {
  JSValue r = JS_Call(g_ctx, fn, JS_UNDEFINED, argc, argv);
  if (JS_IsException(r)) js_report_exception();
  JS_FreeValue(g_ctx, r);
}

// ---------------------------------------------------------------- event bindings

struct EventBinding {
  JSValue fn;         // duped
  JSValue widget;     // duped wrapper, passed as the callback's argument
  lv_obj_t *obj;
  EventBinding *prev, *next;
};
static EventBinding *g_events = nullptr;  // doubly-linked registry

static void event_unlink_and_free(EventBinding *b) {
  if (b->prev) b->prev->next = b->next; else g_events = b->next;
  if (b->next) b->next->prev = b->prev;
  JS_FreeValue(g_ctx, b->fn);
  JS_FreeValue(g_ctx, b->widget);
  free(b);
}

static void event_trampoline(lv_event_t *e) {
  EventBinding *b = static_cast<EventBinding *>(lv_event_get_user_data(e));
  // Pointer events carry the touch point: fn(widget, x, y). Non-pointer
  // dispatches (e.g. a value change set from code) just get fn(widget).
  lv_indev_t *indev = lv_indev_active();
  if (indev) {
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    JSValue args[3] = {b->widget, JS_NewInt32(g_ctx, pt.x), JS_NewInt32(g_ctx, pt.y)};
    js_call_reporting(b->fn, 3, args);
    JS_FreeValue(g_ctx, args[1]);
    JS_FreeValue(g_ctx, args[2]);
  } else {
    js_call_reporting(b->fn, 1, &b->widget);
  }
}

static void event_delete_cb(lv_event_t *e) {
  event_unlink_and_free(static_cast<EventBinding *>(lv_event_get_user_data(e)));
}

// ---------------------------------------------------------------- timer bindings

struct TimerBinding {
  JSValue fn;      // duped
  JSValue self;    // duped timer wrapper — keeps opaque valid while stored
  lv_timer_t *t;
  TimerBinding *prev, *next;
};
static TimerBinding *g_timers = nullptr;

static void timer_release(TimerBinding *b, bool delete_lv_timer) {
  if (b->prev) b->prev->next = b->next; else g_timers = b->next;
  if (b->next) b->next->prev = b->prev;
  if (delete_lv_timer) lv_timer_delete(b->t);
  JS_SetOpaque(b->self, nullptr);  // make a later .stop() a no-op
  JS_FreeValue(g_ctx, b->fn);
  JS_FreeValue(g_ctx, b->self);
  free(b);
}

static void timer_trampoline(lv_timer_t *t) {
  TimerBinding *b = static_cast<TimerBinding *>(lv_timer_get_user_data(t));
  js_call_reporting(b->fn, 0, nullptr);
}

static JSValue js_timer_stop(JSContext *ctx, JSValueConst this_val, int, JSValueConst *) {
  TimerBinding *b = static_cast<TimerBinding *>(JS_GetOpaque(this_val, g_timer_class));
  if (b) timer_release(b, true);
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------- wifi scan state

static struct {
  bool active = false;
  JSValue cb = JS_UNDEFINED;  // duped while active
  lv_timer_t *poll = nullptr;
} g_wifi;

static void wifi_scan_release() {
  if (!g_wifi.active) return;
  g_wifi.active = false;
  if (g_wifi.poll) { lv_timer_delete(g_wifi.poll); g_wifi.poll = nullptr; }
  JS_FreeValue(g_ctx, g_wifi.cb);
  g_wifi.cb = JS_UNDEFINED;
}

static void wifi_poll_timer(lv_timer_t *) {
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;

  JSValue arg;
  if (n < 0) {
    arg = JS_NULL;  // scan failed
  } else {
    arg = JS_NewArray(g_ctx);
    for (int16_t i = 0; i < n; i++) {
      JSValue net = JS_NewObject(g_ctx);
      JS_SetPropertyStr(g_ctx, net, "ssid", JS_NewString(g_ctx, WiFi.SSID(i).c_str()));
      JS_SetPropertyStr(g_ctx, net, "rssi", JS_NewInt32(g_ctx, WiFi.RSSI(i)));
      JS_SetPropertyUint32(g_ctx, arg, i, net);
    }
    WiFi.scanDelete();
  }

  // Release scan state BEFORE the callback runs, so the callback may start a
  // new scan immediately. Keep the fn alive for the call itself.
  JSValue fn = JS_DupValue(g_ctx, g_wifi.cb);
  wifi_scan_release();
  js_call_reporting(fn, 1, &arg);
  JS_FreeValue(g_ctx, fn);
  JS_FreeValue(g_ctx, arg);
}

// ---------------------------------------------------------------- helpers

static lv_obj_t *arg_widget(JSContext *ctx, JSValueConst v) {
  return static_cast<lv_obj_t *>(JS_GetOpaque2(ctx, v, g_widget_class));
}

static JSValue wrap_widget(JSContext *ctx, lv_obj_t *obj) {
  JSValue w = JS_NewObjectClass(ctx, g_widget_class);
  JS_SetOpaque(w, obj);
  return w;
}

static bool parse_color(JSContext *ctx, JSValueConst v, lv_color_t *out) {
  if (JS_IsNumber(v)) {
    uint32_t rgb = 0;
    JS_ToUint32(ctx, &rgb, v);
    *out = lv_color_hex(rgb);
    return true;
  }
  if (JS_IsString(v)) {
    const char *s = JS_ToCString(ctx, v);
    if (!s) return false;
    const char *hex = (s[0] == '#') ? s + 1 : s;
    uint32_t rgb = strtoul(hex, nullptr, 16);
    JS_FreeCString(ctx, s);
    *out = lv_color_hex(rgb);
    return true;
  }
  return false;
}

static const struct { const char *name; lv_align_t code; } kAligns[] = {
    {"center", LV_ALIGN_CENTER},
    {"top-left", LV_ALIGN_TOP_LEFT},
    {"top-mid", LV_ALIGN_TOP_MID},
    {"top-right", LV_ALIGN_TOP_RIGHT},
    {"bottom-left", LV_ALIGN_BOTTOM_LEFT},
    {"bottom-mid", LV_ALIGN_BOTTOM_MID},
    {"bottom-right", LV_ALIGN_BOTTOM_RIGHT},
    {"left-mid", LV_ALIGN_LEFT_MID},
    {"right-mid", LV_ALIGN_RIGHT_MID},
};

static const lv_font_t *font_by_size(int px) {
  switch (px) {
    case 14: return &lv_font_montserrat_14;
    case 16: return &lv_font_montserrat_16;
    case 20: return &lv_font_montserrat_20;
    default: return nullptr;  // only the three compiled-in sizes
  }
}

static void widget_set_text(lv_obj_t *obj, const char *text) {
  if (lv_obj_check_type(obj, &lv_label_class)) {
    lv_label_set_text(obj, text);
    return;
  }
  if (lv_obj_check_type(obj, &lv_button_class)) {
    // Reuse the button's existing label child if it has one, else create it.
    lv_obj_t *lbl = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
      lv_obj_t *c = lv_obj_get_child(obj, i);
      if (lv_obj_check_type(c, &lv_label_class)) { lbl = c; break; }
    }
    if (!lbl) lbl = lv_label_create(obj);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
  }
}

static void widget_set_value(lv_obj_t *obj, JSContext *ctx, JSValueConst v) {
  if (lv_obj_check_type(obj, &lv_slider_class)) {
    int32_t n = 0; JS_ToInt32(ctx, &n, v);
    lv_slider_set_value(obj, n, LV_ANIM_OFF);
  } else if (lv_obj_check_type(obj, &lv_arc_class)) {
    int32_t n = 0; JS_ToInt32(ctx, &n, v);
    lv_arc_set_value(obj, n);
  } else if (lv_obj_check_type(obj, &lv_switch_class)) {
    if (JS_ToBool(ctx, v)) lv_obj_add_state(obj, LV_STATE_CHECKED);
    else lv_obj_remove_state(obj, LV_STATE_CHECKED);
  }
}

// Applies a props object to a widget. Unknown keys are ignored on purpose:
// scripts should degrade, not throw, when running on older firmware.
static void apply_props(JSContext *ctx, lv_obj_t *obj, JSValueConst props) {
  if (!JS_IsObject(props)) return;

  JSValue v;
  int32_t n;

  auto get = [&](const char *k) { return JS_GetPropertyStr(ctx, props, k); };
  auto has = [&](JSValueConst val) { return !JS_IsUndefined(val) && !JS_IsNull(val); };

  v = get("w");
  if (has(v)) {
    if (JS_IsString(v)) lv_obj_set_width(obj, LV_SIZE_CONTENT);
    else { JS_ToInt32(ctx, &n, v); lv_obj_set_width(obj, n); }
  }
  JS_FreeValue(ctx, v);

  v = get("h");
  if (has(v)) {
    if (JS_IsString(v)) lv_obj_set_height(obj, LV_SIZE_CONTENT);
    else { JS_ToInt32(ctx, &n, v); lv_obj_set_height(obj, n); }
  }
  JS_FreeValue(ctx, v);

  // align + x/y offsets are applied together; bare x/y = absolute position.
  {
    JSValue av = get("align"), xv = get("x"), yv = get("y");
    int32_t x = 0, y = 0;
    if (has(xv)) JS_ToInt32(ctx, &x, xv);
    if (has(yv)) JS_ToInt32(ctx, &y, yv);
    if (has(av)) {
      const char *s = JS_ToCString(ctx, av);
      if (s) {
        for (auto &a : kAligns) {
          if (strcmp(s, a.name) == 0) { lv_obj_align(obj, a.code, x, y); break; }
        }
        JS_FreeCString(ctx, s);
      }
    } else if (has(xv) || has(yv)) {
      lv_obj_set_pos(obj, x, y);
    }
    JS_FreeValue(ctx, av); JS_FreeValue(ctx, xv); JS_FreeValue(ctx, yv);
  }

  v = get("text");
  if (has(v)) {
    const char *s = JS_ToCString(ctx, v);
    if (s) { widget_set_text(obj, s); JS_FreeCString(ctx, s); }
  }
  JS_FreeValue(ctx, v);

  v = get("bg");
  if (has(v)) {
    lv_color_t c;
    if (parse_color(ctx, v, &c)) {
      lv_obj_set_style_bg_color(obj, c, 0);
      lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
  }
  JS_FreeValue(ctx, v);

  v = get("color");
  if (has(v)) {
    lv_color_t c;
    if (parse_color(ctx, v, &c)) lv_obj_set_style_text_color(obj, c, 0);
  }
  JS_FreeValue(ctx, v);

  v = get("font");
  if (has(v)) {
    JS_ToInt32(ctx, &n, v);
    const lv_font_t *f = font_by_size(n);
    if (f) lv_obj_set_style_text_font(obj, f, 0);
  }
  JS_FreeValue(ctx, v);

  v = get("range");
  if (has(v)) {
    JSValue lo = JS_GetPropertyUint32(ctx, v, 0), hi = JS_GetPropertyUint32(ctx, v, 1);
    int32_t a = 0, b = 100;
    JS_ToInt32(ctx, &a, lo); JS_ToInt32(ctx, &b, hi);
    JS_FreeValue(ctx, lo); JS_FreeValue(ctx, hi);
    if (lv_obj_check_type(obj, &lv_slider_class)) lv_slider_set_range(obj, a, b);
    else if (lv_obj_check_type(obj, &lv_arc_class)) lv_arc_set_range(obj, a, b);
    else if (lv_obj_check_type(obj, &lv_chart_class))
      lv_chart_set_axis_range(obj, LV_CHART_AXIS_PRIMARY_Y, a, b);
  }
  JS_FreeValue(ctx, v);

  v = get("value");
  if (has(v)) widget_set_value(obj, ctx, v);
  JS_FreeValue(ctx, v);

  v = get("pad");
  if (has(v)) { JS_ToInt32(ctx, &n, v); lv_obj_set_style_pad_all(obj, n, 0); }
  JS_FreeValue(ctx, v);

  v = get("radius");
  if (has(v)) { JS_ToInt32(ctx, &n, v); lv_obj_set_style_radius(obj, n, 0); }
  JS_FreeValue(ctx, v);

  v = get("scroll");
  if (has(v) && !JS_ToBool(ctx, v)) lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  JS_FreeValue(ctx, v);

  v = get("hidden");
  if (has(v)) {
    if (JS_ToBool(ctx, v)) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
  JS_FreeValue(ctx, v);

  v = get("border");
  if (has(v)) { JS_ToInt32(ctx, &n, v); lv_obj_set_style_border_width(obj, n, 0); }
  JS_FreeValue(ctx, v);

  v = get("borderColor");
  if (has(v)) {
    lv_color_t c;
    if (parse_color(ctx, v, &c)) lv_obj_set_style_border_color(obj, c, 0);
  }
  JS_FreeValue(ctx, v);

  v = get("clickable");
  if (has(v)) {
    if (JS_ToBool(ctx, v)) lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  }
  JS_FreeValue(ctx, v);

  // arc-only knobs
  if (lv_obj_check_type(obj, &lv_arc_class)) {
    v = get("rotation");
    if (has(v)) { JS_ToInt32(ctx, &n, v); lv_arc_set_rotation(obj, n); }
    JS_FreeValue(ctx, v);

    v = get("angles");
    if (has(v)) {
      JSValue lo = JS_GetPropertyUint32(ctx, v, 0), hi = JS_GetPropertyUint32(ctx, v, 1);
      int32_t a = 0, b = 360;
      JS_ToInt32(ctx, &a, lo); JS_ToInt32(ctx, &b, hi);
      JS_FreeValue(ctx, lo); JS_FreeValue(ctx, hi);
      lv_arc_set_bg_angles(obj, a, b);
    }
    JS_FreeValue(ctx, v);

    // knob:false turns the arc into a pure indicator (no knob, not touchable),
    // like the C demo's load gauge.
    v = get("knob");
    if (has(v) && !JS_ToBool(ctx, v)) {
      lv_obj_remove_style(obj, nullptr,
                          static_cast<lv_style_selector_t>(LV_PART_KNOB) |
                              static_cast<lv_style_selector_t>(LV_STATE_ANY));
      lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
    JS_FreeValue(ctx, v);
  }

  if (lv_obj_check_type(obj, &lv_chart_class)) {
    v = get("points");
    if (has(v)) { JS_ToInt32(ctx, &n, v); lv_chart_set_point_count(obj, n); }
    JS_FreeValue(ctx, v);

    v = get("divs");
    if (has(v)) {
      JSValue hv = JS_GetPropertyUint32(ctx, v, 0), vv = JS_GetPropertyUint32(ctx, v, 1);
      int32_t a = 0, b = 0;
      JS_ToInt32(ctx, &a, hv); JS_ToInt32(ctx, &b, vv);
      JS_FreeValue(ctx, hv); JS_FreeValue(ctx, vv);
      lv_chart_set_div_line_count(obj, a, b);
    }
    JS_FreeValue(ctx, v);
  }

  if (lv_obj_check_type(obj, &lv_tabview_class)) {
    v = get("bar");
    if (has(v)) { JS_ToInt32(ctx, &n, v); lv_tabview_set_tab_bar_size(obj, n); }
    JS_FreeValue(ctx, v);
  }
}

// ---------------------------------------------------------------- widget methods

static JSValue js_widget_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (argc >= 1) apply_props(ctx, obj, argv[0]);
  return JS_DupValue(ctx, this_val);
}

static const struct { const char *name; lv_event_code_t code; } kEvents[] = {
    {"click", LV_EVENT_CLICKED},
    {"change", LV_EVENT_VALUE_CHANGED},
    {"pressing", LV_EVENT_PRESSING},
    {"press", LV_EVENT_PRESSED},
};

static JSValue js_widget_on(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
    return JS_ThrowTypeError(ctx, "on(event, fn) needs a function");

  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_EXCEPTION;
  lv_event_code_t code = LV_EVENT_ALL;
  for (auto &e : kEvents) {
    if (strcmp(name, e.name) == 0) { code = e.code; break; }
  }
  JS_FreeCString(ctx, name);
  if (code == LV_EVENT_ALL)
    return JS_ThrowTypeError(ctx, "unknown event (use click/change/pressing/press)");

  EventBinding *b = static_cast<EventBinding *>(malloc(sizeof(EventBinding)));
  if (!b) return JS_ThrowOutOfMemory(ctx);
  b->fn = JS_DupValue(ctx, argv[1]);
  b->widget = JS_DupValue(ctx, this_val);
  b->obj = obj;
  b->prev = nullptr;
  b->next = g_events;
  if (g_events) g_events->prev = b;
  g_events = b;

  lv_obj_add_event_cb(obj, event_trampoline, code, b);
  lv_obj_add_event_cb(obj, event_delete_cb, LV_EVENT_DELETE, b);
  return JS_DupValue(ctx, this_val);
}

static JSValue js_widget_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (argc >= 1) {
    widget_set_value(obj, ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
  }
  if (lv_obj_check_type(obj, &lv_slider_class)) return JS_NewInt32(ctx, lv_slider_get_value(obj));
  if (lv_obj_check_type(obj, &lv_arc_class)) return JS_NewInt32(ctx, lv_arc_get_value(obj));
  if (lv_obj_check_type(obj, &lv_switch_class)) return JS_NewBool(ctx, lv_obj_has_state(obj, LV_STATE_CHECKED));
  return JS_UNDEFINED;
}

// list.add(text) -> button wrapper (for .on("click", ...))
static JSValue js_widget_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (!lv_obj_check_type(obj, &lv_list_class))
    return JS_ThrowTypeError(ctx, "add() only works on lv.list widgets");
  const char *s = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : nullptr;
  lv_obj_t *btn = lv_list_add_button(obj, nullptr, s ? s : "");
  if (s) JS_FreeCString(ctx, s);
  return wrap_widget(ctx, btn);
}

// tabview.addTab(name) -> the tab's content container
static JSValue js_widget_add_tab(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (!lv_obj_check_type(obj, &lv_tabview_class))
    return JS_ThrowTypeError(ctx, "addTab() only works on lv.tabview widgets");
  if (argc < 1) return JS_ThrowTypeError(ctx, "addTab(name) needs a name");
  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) return JS_EXCEPTION;
  lv_obj_t *tab = lv_tabview_add_tab(obj, s);
  JS_FreeCString(ctx, s);
  return wrap_widget(ctx, tab);
}

// chart.push(n) — append to the single series, shifting left when full
static JSValue js_widget_push(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (!lv_obj_check_type(obj, &lv_chart_class))
    return JS_ThrowTypeError(ctx, "push() only works on lv.chart widgets");
  int32_t n = 0;
  if (argc >= 1) JS_ToInt32(ctx, &n, argv[0]);
  lv_chart_series_t *ser = static_cast<lv_chart_series_t *>(lv_obj_get_user_data(obj));
  if (ser) lv_chart_set_next_value(obj, ser, n);
  return JS_DupValue(ctx, this_val);
}

// widget.clean() — delete all children (their event bindings are released by
// the LV_EVENT_DELETE hooks)
static JSValue js_widget_clean(JSContext *ctx, JSValueConst this_val, int, JSValueConst *) {
  lv_obj_t *obj = arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  lv_obj_clean(obj);
  return JS_DupValue(ctx, this_val);
}

// widget.bounds() -> {x, y, w, h} of the content area in screen coordinates —
// what you need to place children under a touch point.
static JSValue js_widget_bounds(JSContext *ctx, JSValueConst this_val, int, JSValueConst *) {
  lv_obj_t *obj = arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  lv_obj_update_layout(obj);
  lv_area_t a;
  lv_obj_get_content_coords(obj, &a);
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "x", JS_NewInt32(ctx, a.x1));
  JS_SetPropertyStr(ctx, o, "y", JS_NewInt32(ctx, a.y1));
  JS_SetPropertyStr(ctx, o, "w", JS_NewInt32(ctx, lv_area_get_width(&a)));
  JS_SetPropertyStr(ctx, o, "h", JS_NewInt32(ctx, lv_area_get_height(&a)));
  return o;
}

// ---------------------------------------------------------------- lv namespace

enum WidgetKind { W_OBJ, W_BUTTON, W_LABEL, W_SLIDER, W_SWITCH, W_ARC, W_LIST, W_CHART, W_TABVIEW };

static JSValue js_lv_make(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "widget(parent, props?) needs a parent");
  lv_obj_t *parent = arg_widget(ctx, argv[0]);
  if (!parent) return JS_EXCEPTION;

  lv_obj_t *obj = nullptr;
  switch (magic) {
    case W_OBJ:     obj = lv_obj_create(parent); break;
    case W_BUTTON:  obj = lv_button_create(parent); break;
    case W_LABEL:   obj = lv_label_create(parent); break;
    case W_SLIDER:  obj = lv_slider_create(parent); break;
    case W_SWITCH:  obj = lv_switch_create(parent); break;
    case W_ARC:     obj = lv_arc_create(parent); break;
    case W_LIST:    obj = lv_list_create(parent); break;
    case W_CHART:   obj = lv_chart_create(parent); break;
    case W_TABVIEW: obj = lv_tabview_create(parent); break;
  }
  if (!obj) return JS_ThrowInternalError(ctx, "widget create failed");
  if (argc >= 2) apply_props(ctx, obj, argv[1]);

  if (magic == W_CHART) {
    // v1 charts are single-series line charts in shift mode with hidden point
    // dots — exactly the C demo's heap trace. The series rides in user_data so
    // .push() can find it.
    lv_chart_set_type(obj, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(obj, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_size(obj, 0, 0, LV_PART_INDICATOR);
    lv_color_t sc = lv_palette_main(LV_PALETTE_CYAN);
    if (argc >= 2 && JS_IsObject(argv[1])) {
      JSValue v = JS_GetPropertyStr(ctx, argv[1], "seriesColor");
      if (!JS_IsUndefined(v) && !JS_IsNull(v)) parse_color(ctx, v, &sc);
      JS_FreeValue(ctx, v);
    }
    lv_obj_set_user_data(obj, lv_chart_add_series(obj, sc, LV_CHART_AXIS_PRIMARY_Y));
  }
  return wrap_widget(ctx, obj);
}

static JSValue js_lv_screen(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return wrap_widget(ctx, lv_screen_active());
}

static JSValue js_lv_timer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
    return JS_ThrowTypeError(ctx, "timer(ms, fn) needs a function");
  int32_t ms = 0;
  JS_ToInt32(ctx, &ms, argv[0]);
  if (ms < 10) ms = 10;  // floor: a 0ms JS timer would starve rendering

  TimerBinding *b = static_cast<TimerBinding *>(malloc(sizeof(TimerBinding)));
  if (!b) return JS_ThrowOutOfMemory(ctx);
  JSValue self = JS_NewObjectClass(ctx, g_timer_class);
  JS_SetOpaque(self, b);
  b->fn = JS_DupValue(ctx, argv[1]);
  b->self = JS_DupValue(ctx, self);
  b->t = lv_timer_create(timer_trampoline, ms, b);
  b->prev = nullptr;
  b->next = g_timers;
  if (g_timers) g_timers->prev = b;
  g_timers = b;
  return self;
}

// ---------------------------------------------------------------- console / sys / wifi

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

static JSValue js_sys_heap(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "internal", JS_NewUint32(ctx, heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  JS_SetPropertyStr(ctx, o, "psram", JS_NewUint32(ctx, heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  return o;
}

// Board halves the battery rail before the ADC. GPIO12 is ADC2, which the WiFi
// driver arbitrates — a 0 read means "unavailable", surfaced as null.
static JSValue js_sys_battery(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const uint32_t mv = analogReadMilliVolts(BAT_PIN);
  if (mv == 0) return JS_NULL;
  return JS_NewFloat64(ctx, (mv * 2.0) / 1000.0);
}

static JSValue js_sys_uptime(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return JS_NewUint32(ctx, millis());
}

static JSValue js_sys_fps(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return JS_NewUint32(ctx, jsvm_host_fps());
}

static JSValue js_sys_backlight(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  int32_t pct = 0;
  if (argc >= 1) JS_ToInt32(ctx, &pct, argv[0]);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  jsvm_host_backlight(static_cast<uint8_t>(pct));
  return JS_UNDEFINED;
}

static JSValue js_sys_info(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "model", JS_NewString(ctx, ESP.getChipModel()));
  JS_SetPropertyStr(ctx, o, "rev", JS_NewInt32(ctx, ESP.getChipRevision()));
  JS_SetPropertyStr(ctx, o, "cores", JS_NewInt32(ctx, ESP.getChipCores()));
  JS_SetPropertyStr(ctx, o, "mhz", JS_NewUint32(ctx, getCpuFrequencyMhz()));
  JS_SetPropertyStr(ctx, o, "flashMB", JS_NewUint32(ctx, ESP.getFlashChipSize() / (1024 * 1024)));
  JS_SetPropertyStr(ctx, o, "psramMB", JS_NewUint32(ctx, ESP.getPsramSize() / (1024 * 1024)));
  char lvgl_ver[16];
  snprintf(lvgl_ver, sizeof(lvgl_ver), "%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);
  JS_SetPropertyStr(ctx, o, "lvgl", JS_NewString(ctx, lvgl_ver));
  JS_SetPropertyStr(ctx, o, "quickjs", JS_NewString(ctx, JS_GetVersion()));
  return o;
}

static JSValue js_wifi_scan(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
    return JS_ThrowTypeError(ctx, "scan(fn) needs a function");
  if (g_wifi.active) return JS_NewBool(ctx, false);

  g_wifi.active = true;
  g_wifi.cb = JS_DupValue(ctx, argv[0]);
  WiFi.scanNetworks(true /* async — lv_timer_handler keeps running */);
  g_wifi.poll = lv_timer_create(wifi_poll_timer, 250, nullptr);
  return JS_NewBool(ctx, true);
}

// ---------------------------------------------------------------- setup / teardown

static const JSClassDef kWidgetClassDef = { "LvWidget", nullptr, nullptr, nullptr, nullptr };
static const JSClassDef kTimerClassDef = { "LvTimer", nullptr, nullptr, nullptr, nullptr };

static void install_globals(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);

  // Widget prototype
  JSValue wproto = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, wproto, "set", JS_NewCFunction(ctx, js_widget_set, "set", 1));
  JS_SetPropertyStr(ctx, wproto, "on", JS_NewCFunction(ctx, js_widget_on, "on", 2));
  JS_SetPropertyStr(ctx, wproto, "value", JS_NewCFunction(ctx, js_widget_value, "value", 1));
  JS_SetPropertyStr(ctx, wproto, "add", JS_NewCFunction(ctx, js_widget_add, "add", 1));
  JS_SetPropertyStr(ctx, wproto, "addTab", JS_NewCFunction(ctx, js_widget_add_tab, "addTab", 1));
  JS_SetPropertyStr(ctx, wproto, "push", JS_NewCFunction(ctx, js_widget_push, "push", 1));
  JS_SetPropertyStr(ctx, wproto, "clean", JS_NewCFunction(ctx, js_widget_clean, "clean", 0));
  JS_SetPropertyStr(ctx, wproto, "bounds", JS_NewCFunction(ctx, js_widget_bounds, "bounds", 0));
  JS_SetClassProto(ctx, g_widget_class, wproto);

  // Timer prototype
  JSValue tproto = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, tproto, "stop", JS_NewCFunction(ctx, js_timer_stop, "stop", 0));
  JS_SetClassProto(ctx, g_timer_class, tproto);

  // lv
  JSValue lv = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, lv, "screen", JS_NewCFunction(ctx, js_lv_screen, "screen", 0));
  JS_SetPropertyStr(ctx, lv, "timer", JS_NewCFunction(ctx, js_lv_timer, "timer", 2));
  static const struct { const char *name; WidgetKind kind; } kMakers[] = {
      {"obj", W_OBJ}, {"button", W_BUTTON}, {"label", W_LABEL}, {"slider", W_SLIDER},
      {"switch", W_SWITCH}, {"arc", W_ARC}, {"list", W_LIST}, {"chart", W_CHART},
      {"tabview", W_TABVIEW},
  };
  for (auto &m : kMakers) {
    JS_SetPropertyStr(ctx, lv, m.name,
                      JS_NewCFunctionMagic(ctx, js_lv_make, m.name, 2, JS_CFUNC_generic_magic, m.kind));
  }
  JS_SetPropertyStr(ctx, global, "lv", lv);

  // console
  JSValue console = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
  JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_log, "error", 1));
  JS_SetPropertyStr(ctx, global, "console", console);

  // sys
  JSValue sys = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, sys, "heap", JS_NewCFunction(ctx, js_sys_heap, "heap", 0));
  JS_SetPropertyStr(ctx, sys, "battery", JS_NewCFunction(ctx, js_sys_battery, "battery", 0));
  JS_SetPropertyStr(ctx, sys, "uptime", JS_NewCFunction(ctx, js_sys_uptime, "uptime", 0));
  JS_SetPropertyStr(ctx, sys, "fps", JS_NewCFunction(ctx, js_sys_fps, "fps", 0));
  JS_SetPropertyStr(ctx, sys, "backlight", JS_NewCFunction(ctx, js_sys_backlight, "backlight", 1));
  JS_SetPropertyStr(ctx, sys, "info", JS_NewCFunction(ctx, js_sys_info, "info", 0));
  JS_SetPropertyStr(ctx, global, "sys", sys);

  // wifi
  JSValue wifi = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, wifi, "scan", JS_NewCFunction(ctx, js_wifi_scan, "scan", 1));
  JS_SetPropertyStr(ctx, global, "wifi", wifi);

  JS_FreeValue(ctx, global);
}

bool jsvm_running() { return g_ctx != nullptr; }

bool jsvm_start(const char *src, const char *filename) {
  if (g_rt) jsvm_stop();

  g_rt = JS_NewRuntime2(&kMallocFns, nullptr);
  if (!g_rt) { Serial.println("[js] runtime creation failed"); return false; }
  JS_SetMaxStackSize(g_rt, kJsMaxStack);
  g_ctx = JS_NewContext(g_rt);
  if (!g_ctx) {
    Serial.println("[js] context creation failed");
    JS_FreeRuntime(g_rt);
    g_rt = nullptr;
    return false;
  }

  JS_NewClassID(g_rt, &g_widget_class);
  JS_NewClass(g_rt, g_widget_class, &kWidgetClassDef);
  JS_NewClassID(g_rt, &g_timer_class);
  JS_NewClass(g_rt, g_timer_class, &kTimerClassDef);
  install_globals(g_ctx);

  const uint32_t t0 = millis();
  JSValue r = JS_Eval(g_ctx, src, strlen(src), filename, JS_EVAL_TYPE_GLOBAL);
  const bool ok = !JS_IsException(r);
  if (!ok) js_report_exception();
  JS_FreeValue(g_ctx, r);
  Serial.printf("[js] %s: eval %s in %lu ms\n", filename, ok ? "ok" : "FAILED",
                (unsigned long)(millis() - t0));
  return ok;
}

void jsvm_repl_line(const char *src) {
  if (!g_ctx) {
    Serial.println("[repl] no VM running");
    return;
  }
  JSValue r = JS_Eval(g_ctx, src, strlen(src), "<repl>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) {
    js_report_exception();
  } else {
    const char *s = JS_ToCString(g_ctx, r);
    Serial.printf("[repl] %s\n", s ? s : "(unprintable)");
    JS_FreeCString(g_ctx, s);
  }
  JS_FreeValue(g_ctx, r);
}

void jsvm_pump() {
  if (!g_rt) return;
  JSContext *jctx;
  int r;
  while ((r = JS_ExecutePendingJob(g_rt, &jctx)) > 0) {}
  if (r < 0) js_report_exception();
}

void jsvm_stop() {
  if (!g_rt) return;

  // Order matters — see js_bindings.h. JS-driven lv_timers go first so no
  // callback can fire into a half-dead world.
  wifi_scan_release();
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

  JS_FreeContext(g_ctx);
  JS_FreeRuntime(g_rt);
  g_ctx = nullptr;
  g_rt = nullptr;
  // Class IDs come from a per-runtime counter; zero them so the next runtime
  // allocates fresh ones instead of reusing another runtime's numbering.
  g_widget_class = 0;
  g_timer_class = 0;
}
