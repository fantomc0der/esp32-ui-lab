// bindings_sys.cpp — the `sys` global: device facts and device control.
//
// Nothing here is board-specific. Readings that depend on wiring (backlight,
// battery, frame rate) are forwarded to the host hooks in js_bindings.h; the
// rest are ESP32 facts any board reports. Compile out with -DJSVM_WITH_SYS=0.
//
// This module holds no JS state, so it needs no teardown. The pinned-app path
// below is C state and deliberately outlives every script: it is a device
// setting, not something an app owns.

#include "jsvm_internal.h"

#if JSVM_WITH_SYS

#include <Preferences.h>
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

// Asks the host to run a different script. Returns immediately and the
// current app keeps running until the call stack unwinds — the switch cannot
// happen here, because tearing down the context mid-call would free the very
// closure that is executing. The host performs it from loop().
static JSValue js_sys_launch(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "launch(name) needs a script path");
  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) return JS_EXCEPTION;
  jsvm_request_launch(s);
  JS_FreeCString(ctx, s);
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------- pinning

static const char *kPrefsNamespace = "jsvm-app";

// Cached so the host can ask on every loop() without touching NVS. Empty means
// nothing is pinned; the empty string is never a valid script path.
static char g_pinned[128];
static bool g_pinned_loaded = false;

static void load_pinned() {
  if (g_pinned_loaded) return;
  g_pinned_loaded = true;
  Preferences p;
  if (!p.begin(kPrefsNamespace, true /* read-only */)) return;
  p.getString("pinned", g_pinned, sizeof(g_pinned));
  p.end();
}

const char *jsvm_pinned_app() {
  load_pinned();
  return g_pinned[0] ? g_pinned : nullptr;
}

// Writes through to NVS immediately: pinning is a one-shot user decision, and
// the point of it is to survive the power cycle that comes next.
bool jsvm_set_pinned_app(const char *path) {
  // Refuse an over-long path rather than storing a truncated one that loads
  // nothing.
  if (path && strlen(path) >= sizeof(g_pinned)) return false;
  char next[sizeof(g_pinned)];
  strncpy(next, path ? path : "", sizeof(next) - 1);
  next[sizeof(next) - 1] = '\0';

  load_pinned();  // before the write, or a later read would undo it
  Preferences p;
  if (!p.begin(kPrefsNamespace, false)) return false;
  if (path) p.putString("pinned", next); else p.remove("pinned");
  p.end();
  memcpy(g_pinned, next, sizeof(g_pinned));
  if (path) Serial.printf("[app] pinned %s\n", next);
  else Serial.println("[app] unpinned");
  return true;
}

// Pins a script so the board boots into it instead of the launcher. The switch
// is not made here — pinning says what should run next boot, not now; pair it
// with launch() if you want both.
static JSValue js_sys_pin(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "pin(path) needs a script path");
  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) return JS_EXCEPTION;
  // Same rule the loader and the fs bindings follow, so a pin that succeeds is
  // a path the host can actually load.
  if (*s != '/' && strncmp(s, "flash:/", 7) != 0) {
    JS_FreeCString(ctx, s);
    return JS_ThrowTypeError(ctx, "path must be absolute, e.g. \"/apps/clock.js\"");
  }
  const bool ok = jsvm_set_pinned_app(s);
  JS_FreeCString(ctx, s);
  return JS_NewBool(ctx, ok);
}

static JSValue js_sys_unpin(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return JS_NewBool(ctx, jsvm_set_pinned_app(nullptr));
}

static JSValue js_sys_pinned(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const char *path = jsvm_pinned_app();
  return path ? JS_NewString(ctx, path) : JS_NULL;
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
  JS_SetPropertyStr(ctx, sys, "launch", JS_NewCFunction(ctx, js_sys_launch, "launch", 1));
  JS_SetPropertyStr(ctx, sys, "pin", JS_NewCFunction(ctx, js_sys_pin, "pin", 1));
  JS_SetPropertyStr(ctx, sys, "unpin", JS_NewCFunction(ctx, js_sys_unpin, "unpin", 0));
  JS_SetPropertyStr(ctx, sys, "pinned", JS_NewCFunction(ctx, js_sys_pinned, "pinned", 0));
  JS_SetPropertyStr(ctx, global, "sys", sys);

  JS_FreeValue(ctx, global);
}

#endif  // JSVM_WITH_SYS
