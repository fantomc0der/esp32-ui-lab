// bindings_lv.cpp — the `lv` global: widgets, props, and widget methods.
//
// This module owns vocabulary, not lifetime. Anything that has to outlive a
// call (an event handler, a timer callback) is handed to the core, which dups
// it and frees it at one place; nothing here stores a JSValue.
//
// Adding a widget is three lines: a WidgetKind, a case in js_lv_make's switch,
// and a row in kMakers. All nine share apply_props, which is why this file has
// no per-widget structure — there is nothing per-widget to hold.

#include "jsvm_internal.h"

// ---------------------------------------------------------------- value parsing

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

// Sizes accept a number of pixels, "50%" of the parent's content area, or
// "content" to shrink-wrap children. Percentages are what let a script lay
// out for any resolution instead of one panel's pixel count.
static int32_t parse_size(JSContext *ctx, JSValueConst v) {
  if (JS_IsString(v)) {
    const char *s = JS_ToCString(ctx, v);
    if (!s) return LV_SIZE_CONTENT;
    int32_t size;
    const size_t len = strlen(s);
    if (len && s[len - 1] == '%') {
      size = LV_PCT(static_cast<int32_t>(strtol(s, nullptr, 10)));
    } else {
      size = LV_SIZE_CONTENT;  // "content", and anything else we don't know
    }
    JS_FreeCString(ctx, s);
    return size;
  }
  int32_t n = 0;
  JS_ToInt32(ctx, &n, v);
  return n;
}

static const struct { const char *name; lv_flex_flow_t code; } kFlexFlows[] = {
    {"row", LV_FLEX_FLOW_ROW},
    {"column", LV_FLEX_FLOW_COLUMN},
    {"row-wrap", LV_FLEX_FLOW_ROW_WRAP},
    {"column-wrap", LV_FLEX_FLOW_COLUMN_WRAP},
};

static const struct { const char *name; lv_flex_align_t code; } kFlexAligns[] = {
    {"start", LV_FLEX_ALIGN_START},
    {"end", LV_FLEX_ALIGN_END},
    {"center", LV_FLEX_ALIGN_CENTER},
    {"between", LV_FLEX_ALIGN_SPACE_BETWEEN},
    {"around", LV_FLEX_ALIGN_SPACE_AROUND},
    {"evenly", LV_FLEX_ALIGN_SPACE_EVENLY},
};

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

// Only sizes compiled into the firmware are reachable; anything else is
// ignored so a script degrades rather than throwing. Add a size by enabling
// LV_FONT_MONTSERRAT_<n> in lv_conf.h and adding a case here.
static const lv_font_t *font_by_size(int px) {
  switch (px) {
    case 14: return &lv_font_montserrat_14;
    case 16: return &lv_font_montserrat_16;
    case 20: return &lv_font_montserrat_20;
#if LV_FONT_MONTSERRAT_28
    case 28: return &lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_40
    case 40: return &lv_font_montserrat_40;
#endif
    default: return nullptr;
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
  } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
    // A text field's "value" is its text, which is what a script wants back
    // from .value() after the user has typed.
    const char *s = JS_ToCString(ctx, v);
    if (s) { lv_textarea_set_text(obj, s); JS_FreeCString(ctx, s); }
  }
}

// ---------------------------------------------------------------- props

// Applies a props object to a widget. Unknown keys are ignored on purpose:
// scripts should degrade, not throw, when running on older firmware.
static void apply_props(JSContext *ctx, lv_obj_t *obj, JSValueConst props) {
  if (!JS_IsObject(props)) return;

  JSValue v;
  int32_t n;

  auto get = [&](const char *k) { return JS_GetPropertyStr(ctx, props, k); };
  auto has = [&](JSValueConst val) { return !JS_IsUndefined(val) && !JS_IsNull(val); };

  v = get("w");
  if (has(v)) lv_obj_set_width(obj, parse_size(ctx, v));
  JS_FreeValue(ctx, v);

  v = get("h");
  if (has(v)) lv_obj_set_height(obj, parse_size(ctx, v));
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

  // flex: children lay themselves out, so the parent adapts to any resolution
  // instead of the script hardcoding each child's position.
  v = get("flex");
  if (has(v)) {
    const char *s = JS_ToCString(ctx, v);
    if (s) {
      for (auto &f : kFlexFlows) {
        if (strcmp(s, f.name) == 0) { lv_obj_set_flex_flow(obj, f.code); break; }
      }
      JS_FreeCString(ctx, s);
    }
  }
  JS_FreeValue(ctx, v);

  // flexAlign: "main" or ["main", "cross"] — track alignment follows cross.
  v = get("flexAlign");
  if (has(v)) {
    lv_flex_align_t place[2] = {LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START};
    for (int i = 0; i < 2; i++) {
      JSValue item = JS_IsString(v) ? (i == 0 ? JS_DupValue(ctx, v) : JS_UNDEFINED)
                                    : JS_GetPropertyUint32(ctx, v, i);
      const char *s = JS_IsString(item) ? JS_ToCString(ctx, item) : nullptr;
      if (s) {
        for (auto &a : kFlexAligns) {
          if (strcmp(s, a.name) == 0) { place[i] = a.code; break; }
        }
        JS_FreeCString(ctx, s);
      }
      JS_FreeValue(ctx, item);
    }
    lv_obj_set_flex_align(obj, place[0], place[1], place[1]);
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

  if (lv_obj_check_type(obj, &lv_textarea_class)) {
    v = get("placeholder");
    if (has(v)) {
      const char *s = JS_ToCString(ctx, v);
      if (s) { lv_textarea_set_placeholder_text(obj, s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);

    // Masks typed characters — the reason a password field is not just a label.
    v = get("password");
    if (has(v)) lv_textarea_set_password_mode(obj, JS_ToBool(ctx, v));
    JS_FreeValue(ctx, v);

    v = get("oneLine");
    if (has(v)) lv_textarea_set_one_line(obj, JS_ToBool(ctx, v));
    JS_FreeValue(ctx, v);

    v = get("maxLength");
    if (has(v)) { JS_ToInt32(ctx, &n, v); lv_textarea_set_max_length(obj, n); }
    JS_FreeValue(ctx, v);
  }
}

// ---------------------------------------------------------------- widget methods

static JSValue js_widget_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = jsvm_arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (argc >= 1) apply_props(ctx, obj, argv[0]);
  return JS_DupValue(ctx, this_val);
}

static const struct { const char *name; lv_event_code_t code; } kEvents[] = {
    {"click", LV_EVENT_CLICKED},
    {"change", LV_EVENT_VALUE_CHANGED},
    {"pressing", LV_EVENT_PRESSING},
    {"press", LV_EVENT_PRESSED},
    // Fires while the finger is still down. LVGL sends "click" as well when it
    // lifts, so a handler that wants only the long press has to say so.
    {"longpress", LV_EVENT_LONG_PRESSED},
    // Emitted by the on-screen keyboard's tick and cross keys.
    {"ready", LV_EVENT_READY},
    {"cancel", LV_EVENT_CANCEL},
};

static JSValue js_widget_on(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = jsvm_arg_widget(ctx, this_val);
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
    return JS_ThrowTypeError(ctx, "unknown event (click/change/press/pressing/longpress/ready/cancel)");

  // The core takes ownership of the callback from here.
  if (!jsvm_bind_event(ctx, obj, code, argv[1], this_val))
    return JS_ThrowOutOfMemory(ctx);
  return JS_DupValue(ctx, this_val);
}

static JSValue js_widget_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = jsvm_arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (argc >= 1) {
    widget_set_value(obj, ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
  }
  if (lv_obj_check_type(obj, &lv_slider_class)) return JS_NewInt32(ctx, lv_slider_get_value(obj));
  if (lv_obj_check_type(obj, &lv_arc_class)) return JS_NewInt32(ctx, lv_arc_get_value(obj));
  if (lv_obj_check_type(obj, &lv_switch_class)) return JS_NewBool(ctx, lv_obj_has_state(obj, LV_STATE_CHECKED));
  if (lv_obj_check_type(obj, &lv_textarea_class)) {
    const char *s = lv_textarea_get_text(obj);
    return JS_NewString(ctx, s ? s : "");
  }
  return JS_UNDEFINED;
}

// list.add(text) -> button wrapper (for .on("click", ...))
static JSValue js_widget_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = jsvm_arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (!lv_obj_check_type(obj, &lv_list_class))
    return JS_ThrowTypeError(ctx, "add() only works on lv.list widgets");
  const char *s = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : nullptr;
  lv_obj_t *btn = lv_list_add_button(obj, nullptr, s ? s : "");
  if (s) JS_FreeCString(ctx, s);
  return jsvm_wrap_widget(ctx, btn);
}

// tabview.addTab(name) -> the tab's content container
static JSValue js_widget_add_tab(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = jsvm_arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (!lv_obj_check_type(obj, &lv_tabview_class))
    return JS_ThrowTypeError(ctx, "addTab() only works on lv.tabview widgets");
  if (argc < 1) return JS_ThrowTypeError(ctx, "addTab(name) needs a name");
  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) return JS_EXCEPTION;
  lv_obj_t *tab = lv_tabview_add_tab(obj, s);
  JS_FreeCString(ctx, s);
  return jsvm_wrap_widget(ctx, tab);
}

// chart.push(n) — append to the single series, shifting left when full
static JSValue js_widget_push(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = jsvm_arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (!lv_obj_check_type(obj, &lv_chart_class))
    return JS_ThrowTypeError(ctx, "push() only works on lv.chart widgets");
  int32_t n = 0;
  if (argc >= 1) JS_ToInt32(ctx, &n, argv[0]);
  lv_chart_series_t *ser = static_cast<lv_chart_series_t *>(lv_obj_get_user_data(obj));
  if (ser) lv_chart_set_next_value(obj, ser, n);
  return JS_DupValue(ctx, this_val);
}

// keyboard.target(textarea) — routes typing into that field. LVGL wires the
// key handling, so a script never sees individual keystrokes.
static JSValue js_widget_target(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  lv_obj_t *obj = jsvm_arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  if (!lv_obj_check_type(obj, &lv_keyboard_class))
    return JS_ThrowTypeError(ctx, "target() only works on lv.keyboard widgets");
  if (argc < 1) return JS_ThrowTypeError(ctx, "target(textarea) needs a widget");
  lv_obj_t *ta = jsvm_arg_widget(ctx, argv[0]);
  if (!ta) return JS_EXCEPTION;
  lv_keyboard_set_textarea(obj, ta);
  return JS_DupValue(ctx, this_val);
}

// widget.clean() — delete all children (their event bindings are released by
// the LV_EVENT_DELETE hooks)
static JSValue js_widget_clean(JSContext *ctx, JSValueConst this_val, int, JSValueConst *) {
  lv_obj_t *obj = jsvm_arg_widget(ctx, this_val);
  if (!obj) return JS_EXCEPTION;
  lv_obj_clean(obj);
  return JS_DupValue(ctx, this_val);
}

// widget.bounds() -> {x, y, w, h} of the content area in screen coordinates —
// what you need to place children under a touch point.
static JSValue js_widget_bounds(JSContext *ctx, JSValueConst this_val, int, JSValueConst *) {
  lv_obj_t *obj = jsvm_arg_widget(ctx, this_val);
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

// ---------------------------------------------------------------- constructors

enum WidgetKind { W_OBJ, W_BUTTON, W_LABEL, W_SLIDER, W_SWITCH, W_ARC, W_LIST, W_CHART,
                  W_TABVIEW, W_TEXTAREA, W_KEYBOARD };

static JSValue js_lv_make(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "widget(parent, props?) needs a parent");
  lv_obj_t *parent = jsvm_arg_widget(ctx, argv[0]);
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
    case W_TABVIEW:  obj = lv_tabview_create(parent); break;
    case W_TEXTAREA: obj = lv_textarea_create(parent); break;
    case W_KEYBOARD: obj = lv_keyboard_create(parent); break;
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
  return jsvm_wrap_widget(ctx, obj);
}

static JSValue js_lv_screen(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return jsvm_wrap_widget(ctx, lv_screen_active());
}

// lv.size() -> {w, h} of the active display, in pixels.
//
// Percentages and flex cover most of what a layout needs, but not all of it: a
// chart's point count, how many list rows fit, whether a two-column split is
// worth making at all — those are decisions a script has to make from a number,
// and this is where it gets one. Read once at startup; a panel does not resize.
static JSValue js_lv_size(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  lv_display_t *d = lv_display_get_default();
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "w", JS_NewInt32(ctx, d ? lv_display_get_horizontal_resolution(d) : 0));
  JS_SetPropertyStr(ctx, o, "h", JS_NewInt32(ctx, d ? lv_display_get_vertical_resolution(d) : 0));
  return o;
}

static JSValue js_lv_timer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
    return JS_ThrowTypeError(ctx, "timer(ms, fn) needs a function");
  int32_t ms = 0;
  JS_ToInt32(ctx, &ms, argv[0]);
  return jsvm_create_timer(ctx, ms, argv[1]);  // core owns the callback
}

// ---------------------------------------------------------------- install

void js_install_lv(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);

  // Widget prototype. The class itself is core's, since core hands out and
  // validates the handles.
  JSValue wproto = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, wproto, "set", JS_NewCFunction(ctx, js_widget_set, "set", 1));
  JS_SetPropertyStr(ctx, wproto, "on", JS_NewCFunction(ctx, js_widget_on, "on", 2));
  JS_SetPropertyStr(ctx, wproto, "value", JS_NewCFunction(ctx, js_widget_value, "value", 1));
  JS_SetPropertyStr(ctx, wproto, "add", JS_NewCFunction(ctx, js_widget_add, "add", 1));
  JS_SetPropertyStr(ctx, wproto, "addTab", JS_NewCFunction(ctx, js_widget_add_tab, "addTab", 1));
  JS_SetPropertyStr(ctx, wproto, "push", JS_NewCFunction(ctx, js_widget_push, "push", 1));
  JS_SetPropertyStr(ctx, wproto, "target", JS_NewCFunction(ctx, js_widget_target, "target", 1));
  JS_SetPropertyStr(ctx, wproto, "clean", JS_NewCFunction(ctx, js_widget_clean, "clean", 0));
  JS_SetPropertyStr(ctx, wproto, "bounds", JS_NewCFunction(ctx, js_widget_bounds, "bounds", 0));
  JS_SetClassProto(ctx, jsvm_widget_class, wproto);

  JSValue lv = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, lv, "screen", JS_NewCFunction(ctx, js_lv_screen, "screen", 0));
  JS_SetPropertyStr(ctx, lv, "size", JS_NewCFunction(ctx, js_lv_size, "size", 0));
  JS_SetPropertyStr(ctx, lv, "timer", JS_NewCFunction(ctx, js_lv_timer, "timer", 2));
  static const struct { const char *name; WidgetKind kind; } kMakers[] = {
      {"obj", W_OBJ}, {"button", W_BUTTON}, {"label", W_LABEL}, {"slider", W_SLIDER},
      {"switch", W_SWITCH}, {"arc", W_ARC}, {"list", W_LIST}, {"chart", W_CHART},
      {"tabview", W_TABVIEW}, {"textarea", W_TEXTAREA}, {"keyboard", W_KEYBOARD},
  };
  for (auto &m : kMakers) {
    JS_SetPropertyStr(ctx, lv, m.name,
                      JS_NewCFunctionMagic(ctx, js_lv_make, m.name, 2, JS_CFUNC_generic_magic, m.kind));
  }
  JS_SetPropertyStr(ctx, global, "lv", lv);

  JS_FreeValue(ctx, global);
}
