// bindings_sys.cpp — the `sys` global: device facts and device control.
//
// Nothing here is board-specific. Readings that depend on wiring (backlight,
// battery, frame rate) are forwarded to the host hooks in js_bindings.h; the
// rest are ESP32 facts any board reports. Compile out with -DJSVM_WITH_SYS=0.
//
// This module holds no JS state, so it needs no teardown.

#include "jsvm_internal.h"

#if JSVM_WITH_SYS

#include <esp_heap_caps.h>

static JSValue js_sys_heap(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "internal", JS_NewUint32(ctx, heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  JS_SetPropertyStr(ctx, o, "psram", JS_NewUint32(ctx, heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  return o;
}

// Reading the battery is board wiring, so the host owns it. NAN means the
// reading is unavailable and surfaces to scripts as null rather than a
// confident wrong number.
static JSValue js_sys_battery(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const float volts = jsvm_host_battery();
  if (isnan(volts)) return JS_NULL;
  return JS_NewFloat64(ctx, volts);
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

void js_install_sys(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);

  JSValue sys = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, sys, "heap", JS_NewCFunction(ctx, js_sys_heap, "heap", 0));
  JS_SetPropertyStr(ctx, sys, "battery", JS_NewCFunction(ctx, js_sys_battery, "battery", 0));
  JS_SetPropertyStr(ctx, sys, "uptime", JS_NewCFunction(ctx, js_sys_uptime, "uptime", 0));
  JS_SetPropertyStr(ctx, sys, "fps", JS_NewCFunction(ctx, js_sys_fps, "fps", 0));
  JS_SetPropertyStr(ctx, sys, "backlight", JS_NewCFunction(ctx, js_sys_backlight, "backlight", 1));
  JS_SetPropertyStr(ctx, sys, "info", JS_NewCFunction(ctx, js_sys_info, "info", 0));
  JS_SetPropertyStr(ctx, global, "sys", sys);

  JS_FreeValue(ctx, global);
}

#endif  // JSVM_WITH_SYS
