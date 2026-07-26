// bindings_wifi.cpp — the `wifi` global.
//
// The only module that holds a JS callback across calls, so it is also the
// only one with a teardown. Compile out with -DJSVM_WITH_WIFI=0, which also
// drops WiFi.h from the firmware.
//
// Pattern worth copying for any other async native call: start the operation,
// poll for completion from an lv_timer, and invoke the JS callback from that
// timer. The poll runs on the LVGL task, so the callback lands there too and
// the single-task rule holds without any locking.

#include "jsvm_internal.h"

#if JSVM_WITH_WIFI

#include <WiFi.h>
#include <esp_wifi.h>

static struct {
  bool active = false;
  JSValue cb = JS_UNDEFINED;  // duped while active
  lv_timer_t *poll = nullptr;
} g_wifi;

static void wifi_scan_release() {
  if (!g_wifi.active) return;
  g_wifi.active = false;
  if (g_wifi.poll) { lv_timer_delete(g_wifi.poll); g_wifi.poll = nullptr; }
  JS_FreeValue(jsvm_ctx, g_wifi.cb);
  g_wifi.cb = JS_UNDEFINED;
}

// Teardown can land mid-scan (a reload while the radio is still working), and
// the poll timer that would normally have collected the result is about to
// die. Stopping the scan in the driver matters more than scanDelete() here:
// for a scan still in flight the Arduino result buffer does not exist yet, and
// the scan-done event would allocate it with nobody left to free it. Both
// calls are harmless when there is nothing to release.
void js_teardown_wifi() {
  if (!g_wifi.active) return;
  wifi_scan_release();
  esp_wifi_scan_stop();
  WiFi.scanDelete();
}

static void wifi_poll_timer(lv_timer_t *) {
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;

  JSValue arg;
  if (n < 0) {
    arg = JS_NULL;  // scan failed
  } else {
    arg = JS_NewArray(jsvm_ctx);
    for (int16_t i = 0; i < n; i++) {
      JSValue net = JS_NewObject(jsvm_ctx);
      JS_SetPropertyStr(jsvm_ctx, net, "ssid", JS_NewString(jsvm_ctx, WiFi.SSID(i).c_str()));
      JS_SetPropertyStr(jsvm_ctx, net, "rssi", JS_NewInt32(jsvm_ctx, WiFi.RSSI(i)));
      JS_SetPropertyUint32(jsvm_ctx, arg, i, net);
    }
    WiFi.scanDelete();
  }

  // Release scan state BEFORE the callback runs, so the callback may start a
  // new scan immediately. Keep the fn alive for the call itself.
  JSValue fn = JS_DupValue(jsvm_ctx, g_wifi.cb);
  wifi_scan_release();
  jsvm_call_reporting(fn, 1, &arg);
  JS_FreeValue(jsvm_ctx, fn);
  JS_FreeValue(jsvm_ctx, arg);
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

void js_install_wifi(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);

  JSValue wifi = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, wifi, "scan", JS_NewCFunction(ctx, js_wifi_scan, "scan", 1));
  JS_SetPropertyStr(ctx, global, "wifi", wifi);

  JS_FreeValue(ctx, global);
}

#endif  // JSVM_WITH_WIFI
