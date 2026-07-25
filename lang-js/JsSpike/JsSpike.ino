// JsSpike - Phase 1 go/no-go gate for JS scripting on the ESP32-S3-Touch-LCD-1.47.
//
// Proves the QuickJS-ng interpreter (vendored in this folder, v0.15.1) runs on this
// board with its heap in PSRAM. Serial only - no display, no LVGL. See
// docs/lang-js/js-scripting-plan.md for the full plan this gates.
//
// What it does at boot:
//   1. snapshots free internal RAM + PSRAM
//   2. creates a QuickJS runtime whose allocator is heap_caps_malloc(MALLOC_CAP_SPIRAM)
//   3. runs a small eval suite (arithmetic, closures, GC churn) with timings
//   4. prints the memory deltas that prove the JS heap landed in PSRAM
// Then loop() is a one-line-at-a-time serial REPL: type JS, get the result back.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "quickjs.h"

// QuickJS recurses on the C stack while parsing/executing. The default loopTask
// stack (8 KB) is too small; give it 32 KB and tell QuickJS to bail before that.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);
static const size_t kJsMaxStack = 20 * 1024;

// ---- allocator: route the whole JS heap to PSRAM ---------------------------

static void *qjs_calloc(void *opaque, size_t count, size_t size) {
  return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM);
}
static void *qjs_malloc(void *opaque, size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}
static void qjs_free(void *opaque, void *ptr) {
  heap_caps_free(ptr);
}
static void *qjs_realloc(void *opaque, void *ptr, size_t size) {
  return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
}
// QuickJS treats usable_size as *writable* capacity and will fill it to the
// byte. With IDF heap poisoning enabled, heap_caps_get_allocated_size() counts
// the tail-canary region too, so reporting it lets JS string code overwrite
// the canary and abort() the chip. 0 = "unknown", the safe upstream default.
static size_t qjs_usable_size(const void *ptr) {
  return 0;
}

static const JSMallocFunctions kMallocFns = {
  qjs_calloc, qjs_malloc, qjs_free, qjs_realloc, qjs_usable_size,
};

static JSRuntime *rt = nullptr;
static JSContext *ctx = nullptr;

// ---- tiny native binding so scripts can talk back --------------------------

static JSValue js_print(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
  for (int i = 0; i < argc; i++) {
    const char *s = JS_ToCString(c, argv[i]);
    if (s) {
      if (i) Serial.print(' ');
      Serial.print(s);
      JS_FreeCString(c, s);
    }
  }
  Serial.println();
  return JS_UNDEFINED;
}

// ---- eval helper -----------------------------------------------------------

static void evalAndReport(const char *src, const char *label) {
  uint32_t t0 = micros();
  JSValue v = JS_Eval(ctx, src, strlen(src), "<spike>", JS_EVAL_TYPE_GLOBAL);
  uint32_t dt = micros() - t0;
  if (JS_IsException(v)) {
    JSValue exc = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, exc);
    Serial.printf("[eval] %s -> EXCEPTION: %s (%lu us)\n", label, s ? s : "?", (unsigned long)dt);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, exc);
  } else {
    const char *s = JS_ToCString(ctx, v);
    Serial.printf("[eval] %s -> %s (%lu us)\n", label, s ? s : "?", (unsigned long)dt);
    JS_FreeCString(ctx, s);
  }
  JS_FreeValue(ctx, v);
}

static void printMem(const char *tag) {
  Serial.printf("[mem] %s: internal free %u, psram free %u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < 4000) delay(10);  // native USB needs a beat
  delay(300);

  Serial.println("\n[boot] JsSpike starting");
  Serial.printf("[boot] quickjs-ng %s\n", JS_GetVersion());
  printMem("before runtime");

  rt = JS_NewRuntime2(&kMallocFns, nullptr);
  if (!rt) { Serial.println("[boot] JS_NewRuntime2 FAILED"); return; }
  JS_SetMaxStackSize(rt, kJsMaxStack);
  ctx = JS_NewContext(rt);
  if (!ctx) { Serial.println("[boot] JS_NewContext FAILED"); return; }

  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
  JS_FreeValue(ctx, global);

  printMem("after runtime+context");

  evalAndReport("1+1", "1+1");
  evalAndReport("[1,2,3,4].map(x => x*x).join(',')", "map/arrow");
  evalAndReport(
      "function counter(){ let n=0; return () => ++n; }"
      "const c = counter(); c(); c(); c()",
      "closures");
  evalAndReport(
      "let a=[]; for (let i=0;i<20000;i++) a.push({i, s:'x'+i}); "
      "a.length + ':' + a[19999].s",
      "alloc 20k objects");
  printMem("after 20k-object eval");
  evalAndReport("JSON.stringify({board:'esp32-s3', px:[172,320]})", "JSON");
  evalAndReport("print('hello from JS'); 'print ok'", "native print()");

  // Drop the garbage from the churn test, then show what the VM really holds.
  evalAndReport("a = null; 'released'", "release refs");
  JS_RunGC(rt);
  printMem("after JS_RunGC");

  JSMemoryUsage mu;
  JS_ComputeMemoryUsage(rt, &mu);
  Serial.printf("[mem] VM self-report: malloc_size %lld, malloc_count %lld\n",
                (long long)mu.malloc_size, (long long)mu.malloc_count);

  Serial.println("[boot] ready - type JS on one line, get eval result back");
}

// ---- serial REPL -----------------------------------------------------------

void loop() {
  static String line;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (ctx && line.length()) evalAndReport(line.c_str(), "repl");
      line = "";
    } else {
      line += ch;
    }
  }
  delay(10);
}
