// bindings_wifi.cpp — the `wifi` global and `fetch()`.
//
// Owns everything network: joining an access point, remembering credentials,
// staying joined, and HTTP. Compile out with -DJSVM_WITH_WIFI=0, which also
// drops WiFi.h and HTTPClient from the firmware.
//
// Two rules shape this file.
//
// Credentials are write-only from JavaScript. wifi.save() stores them in NVS
// and wifi.status() reports the SSID, but nothing hands the password back, so
// a script loaded off a card cannot read the network password out of the
// device. NVS is not encrypted, so this guards against a rogue script, not
// against someone holding the board.
//
// Blocking work never runs on the LVGL task. An HTTP request takes hundreds of
// milliseconds to seconds — DNS, TCP, TLS, transfer — and doing that inline
// would freeze rendering and touch. fetch() hands the request to a worker task
// and collects the result from an lv_timer, so the JS callback still lands on
// the one task allowed to touch the VM.

#include "jsvm_internal.h"

#if JSVM_WITH_WIFI

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ---------------------------------------------------------------- credentials

static const char *kPrefsNamespace = "jsvm-wifi";
static bool g_events_hooked = false;
// Cleared by wifi.forget() so the reconnect handler stops fighting the user.
static bool g_want_connection = false;

// Retrying is supervised from an lv_timer rather than driven straight from the
// WiFi event, for two reasons. Events arrive on the system event task, so
// creating or touching anything LVGL owns from there would break the
// single-task rule. And reconnecting the instant a disconnect arrives turns a
// wrong password into a continuous spin — the driver fails, fires the event,
// and we immediately ask again. The handler therefore only records what
// happened; the timer, on the LVGL task, decides when to try again.
static volatile uint8_t g_last_reason = 0;
static uint32_t g_next_attempt = 0;
static uint8_t g_attempts = 0;
static lv_timer_t *g_supervisor = nullptr;

// The distinction that matters when nothing connects: a typo versus a network
// that isn't there.
static const char *reason_name(uint8_t r) {
  switch (r) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:            return "network not found";
    case WIFI_REASON_ASSOC_FAIL:             return "association failed";
    case WIFI_REASON_BEACON_TIMEOUT:         return "lost the access point";
    default:                                 return "disconnected";
  }
}

static void wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[wifi] connected, ip %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // Recorded only. Acting on it here would mean both touching LVGL from
      // the wrong task and retrying with no delay.
      g_last_reason = info.wifi_sta_disconnected.reason;
      break;
    default:
      break;
  }
}

// Runs on the LVGL task. The ESP32 does not retry on its own — a router reboot
// or an expired DHCP lease otherwise leaves it off the network indefinitely —
// so this keeps asking, with a widening gap so a permanent failure costs an
// occasional line rather than a flood.
static void supervise(lv_timer_t *) {
  if (!g_want_connection) return;
  if (WiFi.status() == WL_CONNECTED) {
    g_attempts = 0;
    return;
  }
  const uint32_t now = millis();
  if (now < g_next_attempt) return;

  const uint8_t shift = g_attempts < 5 ? g_attempts : 5;
  uint32_t backoff = 2000u << shift;   // 2s, 4s, 8s, 16s, 32s, then 64s
  if (backoff > 60000u) backoff = 60000u;
  g_next_attempt = now + backoff;

  // Keep retrying forever (the network may come back) but stop narrating it.
  if (g_attempts < 10) {
    Serial.printf("[wifi] %s — attempt %u, next in %lus\n", reason_name(g_last_reason),
                  g_attempts + 1, (unsigned long)(backoff / 1000));
  }
  g_attempts++;
  WiFi.reconnect();
}

static void ensure_events() {
  if (g_events_hooked) return;
  WiFi.onEvent(wifi_event);
  g_events_hooked = true;
}

// Must be called after lv_init(): it creates the supervisor timer. The
// supervisor deliberately outlives app teardown, since the connection should
// survive switching apps; it touches only WiFi and Serial, never the VM.
static void start_connection(const char *ssid, const char *pass) {
  ensure_events();
  g_want_connection = true;
  g_attempts = 0;
  g_next_attempt = millis() + 2000;  // let the initial begin() settle first
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  if (!g_supervisor) g_supervisor = lv_timer_create(supervise, 1000, nullptr);
}

// Joins with whatever is stored. Safe when nothing is; returns false then.
bool jsvm_wifi_autoconnect() {
  Preferences p;
  if (!p.begin(kPrefsNamespace, true /* read-only */)) return false;
  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
  p.end();
  if (ssid.isEmpty()) return false;

  start_connection(ssid.c_str(), pass.c_str());
  Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
  return true;
}

static JSValue js_wifi_save(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "save(ssid, password) needs an ssid");
  const char *ssid = JS_ToCString(ctx, argv[0]);
  if (!ssid) return JS_EXCEPTION;
  const char *pass = (argc >= 2 && !JS_IsUndefined(argv[1])) ? JS_ToCString(ctx, argv[1]) : nullptr;

  Preferences p;
  const bool ok = p.begin(kPrefsNamespace, false);
  if (ok) {
    p.putString("ssid", ssid);
    p.putString("pass", pass ? pass : "");
    p.end();
    start_connection(ssid, pass ? pass : "");
    Serial.printf("[wifi] saved, connecting to %s\n", ssid);
  }

  JS_FreeCString(ctx, ssid);
  if (pass) JS_FreeCString(ctx, pass);
  return JS_NewBool(ctx, ok);
}

static JSValue js_wifi_forget(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  Preferences p;
  if (p.begin(kPrefsNamespace, false)) {
    p.clear();
    p.end();
  }
  g_want_connection = false;
  WiFi.disconnect();
  Serial.println("[wifi] credentials cleared");
  return JS_UNDEFINED;
}

static JSValue js_wifi_connect(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return JS_NewBool(ctx, jsvm_wifi_autoconnect());
}

// Deliberately never reports the password.
static JSValue js_wifi_status(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const bool up = WiFi.status() == WL_CONNECTED;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "connected", JS_NewBool(ctx, up));
  JS_SetPropertyStr(ctx, o, "ssid", JS_NewString(ctx, up ? WiFi.SSID().c_str() : ""));
  JS_SetPropertyStr(ctx, o, "ip", JS_NewString(ctx, up ? WiFi.localIP().toString().c_str() : ""));
  JS_SetPropertyStr(ctx, o, "rssi", JS_NewInt32(ctx, up ? WiFi.RSSI() : 0));
  JS_SetPropertyStr(ctx, o, "saved", JS_NewBool(ctx, g_want_connection));
  return o;
}

// ---------------------------------------------------------------- scan

static struct {
  bool active = false;
  JSValue cb = JS_UNDEFINED;  // duped while active
  lv_timer_t *poll = nullptr;
} g_scan;

static void scan_release() {
  if (!g_scan.active) return;
  g_scan.active = false;
  if (g_scan.poll) { lv_timer_delete(g_scan.poll); g_scan.poll = nullptr; }
  JS_FreeValue(jsvm_ctx, g_scan.cb);
  g_scan.cb = JS_UNDEFINED;
}

static void scan_poll_timer(lv_timer_t *) {
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
      JS_SetPropertyStr(jsvm_ctx, net, "open",
                        JS_NewBool(jsvm_ctx, WiFi.encryptionType(i) == WIFI_AUTH_OPEN));
      JS_SetPropertyUint32(jsvm_ctx, arg, i, net);
    }
    WiFi.scanDelete();
  }

  // Release scan state BEFORE the callback runs, so the callback may start a
  // new scan immediately. Keep the fn alive for the call itself.
  JSValue fn = JS_DupValue(jsvm_ctx, g_scan.cb);
  scan_release();
  jsvm_call_reporting(fn, 1, &arg);
  JS_FreeValue(jsvm_ctx, fn);
  JS_FreeValue(jsvm_ctx, arg);
}

static JSValue js_wifi_scan(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
    return JS_ThrowTypeError(ctx, "scan(fn) needs a function");
  if (g_scan.active) return JS_NewBool(ctx, false);

  g_scan.active = true;
  g_scan.cb = JS_DupValue(ctx, argv[0]);
  WiFi.scanNetworks(true /* async — lv_timer_handler keeps running */);
  g_scan.poll = lv_timer_create(scan_poll_timer, 250, nullptr);
  return JS_NewBool(ctx, true);
}

// ---------------------------------------------------------------- fetch

struct FetchResult {
  uint32_t generation;
  int status;  // HTTP status, or one of the negative codes below
  char *body;  // PSRAM, owned by whoever pops it off the queue
  size_t len;
};

static const int kErrBadUrl = -1000;
static const int kErrTooLarge = -1001;
static const size_t kMaxBody = 128 * 1024;

static QueueHandle_t g_fetch_q = nullptr;
static lv_timer_t *g_fetch_poll = nullptr;
static bool g_fetch_busy = false;
static JSValue g_fetch_resolve = JS_UNDEFINED;
static JSValue g_fetch_reject = JS_UNDEFINED;
static char g_fetch_url[512];

// Bumped whenever a request is abandoned. A worker that finishes afterwards
// still pushes its result, and the poll discards anything whose generation no
// longer matches — which is why the worker never needs to be killed mid-flight.
static uint32_t g_generation = 1;

static void fetch_release() {
  if (g_fetch_poll) { lv_timer_delete(g_fetch_poll); g_fetch_poll = nullptr; }
  if (!JS_IsUndefined(g_fetch_resolve)) JS_FreeValue(jsvm_ctx, g_fetch_resolve);
  if (!JS_IsUndefined(g_fetch_reject)) JS_FreeValue(jsvm_ctx, g_fetch_reject);
  g_fetch_resolve = JS_UNDEFINED;
  g_fetch_reject = JS_UNDEFINED;
  g_fetch_busy = false;
}

// Runs on its own task. Touches nothing the VM owns: only the URL captured
// before it started, and the queue on the way out.
static void fetch_worker(void *arg) {
  const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  FetchResult *r = static_cast<FetchResult *>(calloc(1, sizeof(FetchResult)));
  if (r) {
    r->generation = generation;
    HTTPClient http;
    bool begun;
    if (strncmp(g_fetch_url, "https:", 6) == 0) {
      static NetworkClientSecure tls;
      // No certificate store on the device, so this trusts the network it is
      // on — the same trust level as the plain-HTTP case, and appropriate for
      // a LAN gadget rather than anything handling secrets.
      tls.setInsecure();
      begun = http.begin(tls, g_fetch_url);
    } else {
      begun = http.begin(g_fetch_url);
    }

    if (!begun) {
      r->status = kErrBadUrl;
    } else {
      http.setTimeout(10000);
      r->status = http.GET();
      if (r->status > 0) {
        String body = http.getString();
        if (body.length() > kMaxBody) {
          r->status = kErrTooLarge;
        } else {
          r->body = static_cast<char *>(heap_caps_malloc(body.length() + 1, MALLOC_CAP_SPIRAM));
          if (r->body) {
            memcpy(r->body, body.c_str(), body.length() + 1);
            r->len = body.length();
          }
        }
      }
      http.end();
    }

    if (xQueueSend(g_fetch_q, &r, 0) != pdTRUE) {
      if (r->body) heap_caps_free(r->body);
      free(r);
    }
  }
  vTaskDelete(nullptr);
}

static void fetch_poll_timer(lv_timer_t *) {
  FetchResult *r = nullptr;
  if (xQueueReceive(g_fetch_q, &r, 0) != pdTRUE || !r) return;

  // A result from an abandoned request: drop it and leave the current state.
  if (r->generation != g_generation) {
    if (r->body) heap_caps_free(r->body);
    free(r);
    return;
  }

  JSValue fn, arg;
  if (r->status > 0) {
    fn = JS_DupValue(jsvm_ctx, g_fetch_resolve);
    arg = JS_NewObject(jsvm_ctx);
    JS_SetPropertyStr(jsvm_ctx, arg, "status", JS_NewInt32(jsvm_ctx, r->status));
    JS_SetPropertyStr(jsvm_ctx, arg, "ok",
                      JS_NewBool(jsvm_ctx, r->status >= 200 && r->status < 300));
    JS_SetPropertyStr(jsvm_ctx, arg, "body",
                      r->body ? JS_NewStringLen(jsvm_ctx, r->body, r->len)
                              : JS_NewString(jsvm_ctx, ""));
  } else {
    fn = JS_DupValue(jsvm_ctx, g_fetch_reject);
    const char *why = (r->status == kErrTooLarge) ? "response too large"
                    : (r->status == kErrBadUrl)   ? "bad url"
                                                  : "request failed";
    arg = JS_NewError(jsvm_ctx);
    JS_SetPropertyStr(jsvm_ctx, arg, "message", JS_NewString(jsvm_ctx, why));
    JS_SetPropertyStr(jsvm_ctx, arg, "status", JS_NewInt32(jsvm_ctx, r->status));
  }

  if (r->body) heap_caps_free(r->body);
  free(r);

  // Release before settling: the .then() handler may start another fetch.
  fetch_release();
  jsvm_call_reporting(fn, 1, &arg);
  JS_FreeValue(jsvm_ctx, fn);
  JS_FreeValue(jsvm_ctx, arg);
}

static JSValue js_fetch(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "fetch(url) needs a url");
  if (g_fetch_busy) return JS_ThrowInternalError(ctx, "a fetch is already in flight");
  if (WiFi.status() != WL_CONNECTED) return JS_ThrowInternalError(ctx, "not connected to wifi");

  const char *url = JS_ToCString(ctx, argv[0]);
  if (!url) return JS_EXCEPTION;
  strncpy(g_fetch_url, url, sizeof(g_fetch_url) - 1);
  g_fetch_url[sizeof(g_fetch_url) - 1] = '\0';
  JS_FreeCString(ctx, url);

  if (!g_fetch_q) g_fetch_q = xQueueCreate(4, sizeof(FetchResult *));
  if (!g_fetch_q) return JS_ThrowOutOfMemory(ctx);

  JSValue funcs[2];
  JSValue promise = JS_NewPromiseCapability(ctx, funcs);
  if (JS_IsException(promise)) return promise;
  g_fetch_resolve = funcs[0];
  g_fetch_reject = funcs[1];
  g_fetch_busy = true;

  // TLS is the reason for the stack size; plain HTTP needs far less.
  if (xTaskCreate(fetch_worker, "jsfetch", 16384,
                  reinterpret_cast<void *>(static_cast<uintptr_t>(g_generation)),
                  tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    fetch_release();
    JS_FreeValue(ctx, promise);
    return JS_ThrowInternalError(ctx, "could not start the fetch task");
  }
  g_fetch_poll = lv_timer_create(fetch_poll_timer, 50, nullptr);
  return promise;
}

// ---------------------------------------------------------------- lifecycle

void js_teardown_wifi() {
  // Abandon any in-flight fetch. The worker may still be running, so bump the
  // generation instead of killing it: its result becomes a no-op.
  if (g_fetch_busy) {
    g_generation++;
    fetch_release();
  }
  if (g_scan.active) {
    scan_release();
    esp_wifi_scan_stop();
    WiFi.scanDelete();
  }
}

void js_install_wifi(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);

  JSValue wifi = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, wifi, "scan", JS_NewCFunction(ctx, js_wifi_scan, "scan", 1));
  JS_SetPropertyStr(ctx, wifi, "status", JS_NewCFunction(ctx, js_wifi_status, "status", 0));
  JS_SetPropertyStr(ctx, wifi, "save", JS_NewCFunction(ctx, js_wifi_save, "save", 2));
  JS_SetPropertyStr(ctx, wifi, "connect", JS_NewCFunction(ctx, js_wifi_connect, "connect", 0));
  JS_SetPropertyStr(ctx, wifi, "forget", JS_NewCFunction(ctx, js_wifi_forget, "forget", 0));
  JS_SetPropertyStr(ctx, global, "wifi", wifi);

  JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));

  JS_FreeValue(ctx, global);
}

#endif  // JSVM_WITH_WIFI
