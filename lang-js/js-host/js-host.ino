// js-host — JavaScript-scripted LVGL runtime for the Waveshare
// ESP32-S3-Touch-LCD-1.47.
//
// The firmware is C; the UI is not. At boot this sketch brings up the display,
// touch, and LVGL exactly like the proven C demo (lang-c/app), then hands the
// screen to JavaScript executed by QuickJS-ng.
//
// The board runs one script at a time and boots into /app.js, the launcher,
// which lists the other scripts on the card and starts whichever you tap. A
// script asks to switch with sys.launch(), and there are two ways back that no
// app can break: the home button the firmware draws on LVGL's top layer, and a
// long-press of BOOT. Every switch is queued and performed from loop(), never
// from inside a callback (see requestApp).
//
// A missing or throwing app falls back to the launcher; a missing launcher
// falls back to a screen built into the firmware, so the panel is never dead.
//
// The serial port is a JS REPL into the running app plus a few host commands
// (home, reload, ls, rm, app-begin/app-end) — see pollSerialRepl().
//
// The JS engine and the LVGL bindings are libraries beside this sketch
// (../quickjs-ng, ../lvgl-js-bindings); what stays here is the hardware
// bring-up plus the policy choices: where scripts come from, what triggers a
// reload, and what the serial port does. Build with lang-js/flash.ps1.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>

#include <js_bindings.h>

#include "board_config.h"
#include "board_display.h"
#include "board_storage.h"
#include "js_fallback.h"
#include "touch.h"

// QuickJS recurses on the C stack; the default 8 KB loopTask stack is too
// small. The VM's own limit (JS_MAX_STACK, passed by flash.ps1) must stay well
// under this — QuickJS derives its overflow threshold by subtracting that limit
// from the current stack pointer, so a limit close to the real stack size makes
// every eval fail on entry.
SET_LOOP_TASK_STACK_SIZE(BOARD_LOOP_STACK_KB * 1024);

// ---------------------------------------------------------------- display stack
// The panel-specific parts (which controller, what offsets, how it resets) live
// in board_display.h. On the Waveshare target those constants are identical to
// lang-c/app — see that sketch and docs/lang-c/ for the reasoning behind them.

Arduino_DataBus *bus = board_make_bus();
Arduino_GFX *gfx = board_make_gfx(bus);

// RGB565 = 2 bytes/px on the wire; never size buffers with sizeof(lv_color_t)
// (3 in LVGL 9).
static constexpr uint32_t kBytesPerPx = 2;
// Buffer height is a fraction of the screen, set per target: bigger buffers mean
// fewer flush calls, smaller ones leave more RAM for the JS heap. Boards without
// PSRAM trade FPS for that headroom.
static constexpr uint32_t kBufBytes =
    SCREEN_W * (SCREEN_H / BOARD_DRAW_BUF_DIVISOR) * kBytesPerPx;
static uint8_t *lv_buf_a = nullptr;
static uint8_t *lv_buf_b = nullptr;

static uint32_t flush_count = 0;
static uint32_t fps_window_start = 0;
static uint32_t fps_value = 0;

static uint32_t lv_tick_cb() { return millis(); }

static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area,
                        uint8_t *px_map) {
  const uint32_t w = lv_area_get_width(area);
  const uint32_t h = lv_area_get_height(area);
  // LVGL renders RGB565 little-endian; the panel wants big-endian. Exactly one
  // byte swap happens in this pipeline, and it is this one.
  lv_draw_sw_rgb565_swap(px_map, w * h);
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1,
                            reinterpret_cast<uint16_t *>(px_map), w, h);
  flush_count++;
  lv_display_flush_ready(disp);
}

static void lv_touch_cb(lv_indev_t *, lv_indev_data_t *data) {
  uint16_t x = 0, y = 0;
  if (touch_read(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ---------------------------------------------------------------- host hooks

static void setBacklight(uint8_t percent) {
  if (percent > 100) percent = 100;
  // Floor of 12/255: 0% looks identical to a crashed board.
  analogWrite(LCD_PIN_BL, map(percent, 0, 100, 12, 255));
}

// The three hooks the binding library calls for anything board-specific.
uint32_t jsvm_host_fps() { return fps_value; }
void jsvm_host_backlight(uint8_t percent) { setBacklight(percent); }

// Battery sense exists only on boards wired for it. NAN reaches scripts as
// null, which is also what a board without the circuit reports.
float jsvm_host_battery() {
#if BOARD_HAS_BATTERY
  // This board halves the battery rail before the ADC. GPIO12 is an ADC2
  // channel, and the WiFi driver arbitrates ADC2, so a 0 reading means
  // "unavailable" rather than "flat".
  const uint32_t mv = analogReadMilliVolts(BAT_PIN);
  if (mv == 0) return NAN;
  return (mv * 2.0f) / 1000.0f;
#else
  return NAN;
#endif
}

// ---------------------------------------------------------------- script loading

// Reads a whole file into a bulk buffer (caller frees). nullptr on any miss.
// BOARD_BULK_CAPS is PSRAM where there is any and byte-addressable internal
// DRAM where there is not.
static char *readAll(fs::FS &fs, const char *path) {
  File f = fs.open(path, FILE_READ);
  if (!f || f.isDirectory()) return nullptr;
  const size_t n = f.size();
  char *buf = static_cast<char *>(heap_caps_malloc(n + 1, BOARD_BULK_CAPS));
  if (!buf) { f.close(); return nullptr; }
  const size_t got = f.read(reinterpret_cast<uint8_t *>(buf), n);
  f.close();
  buf[got] = '\0';
  return buf;
}

// The launcher: the script that lists the others. Everything else is reached
// from it, and it is where a failed app lands you.
static const char *kLauncher = "/app.js";

static char g_current_app[128] = "";
static char g_next_app[128] = "";
static lv_obj_t *g_home_btn = nullptr;

// Queues an app switch. Every path that changes apps goes through here — a
// script's sys.launch(), the home button, the BOOT button — so the actual
// teardown always happens from loop() and never from inside a callback that
// the teardown would pull the ground out from under.
static void requestApp(const char *path) {
  strncpy(g_next_app, path, sizeof(g_next_app) - 1);
  g_next_app[sizeof(g_next_app) - 1] = '\0';
}

// "flash:/x" reads the FATFS partition explicitly. Any other path prefers the
// card and falls back to flash, so the same script paths work whether or not a
// card is fitted.
static char *loadScript(const char *path) {
  fs::FS *sd = board_storage::sd();
  fs::FS *flash = board_storage::flash();

  if (strncmp(path, "flash:", 6) == 0) {
    return flash ? readAll(*flash, path + 6) : nullptr;
  }
  char *src = sd ? readAll(*sd, path) : nullptr;
  if (!src && flash) src = readAll(*flash, path);
  return src;
}

static void showHomeButton(bool visible) {
  if (!g_home_btn) return;
  if (visible) lv_obj_remove_flag(g_home_btn, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(g_home_btn, LV_OBJ_FLAG_HIDDEN);
}

// Starts a script. A missing or throwing app falls back to the launcher, and
// a broken launcher falls back to the built-in screen, so there is no way to
// end up staring at a dead panel. Recursion is bounded at one level: the
// retry always targets kLauncher, which takes the built-in branch if it fails.
static void runApp(const char *path) {
  jsvm_stop();

  char *src = loadScript(path);
  bool ok = false;
  if (src) {
    Serial.printf("[app] running %s\n", path);
    ok = jsvm_start(src, path);
    heap_caps_free(src);
  } else {
    Serial.printf("[app] %s not found\n", path);
  }

  if (ok) {
    strncpy(g_current_app, path, sizeof(g_current_app) - 1);
    g_current_app[sizeof(g_current_app) - 1] = '\0';
  } else {
    jsvm_stop();
    if (strcmp(path, kLauncher) != 0) {
      Serial.println("[app] falling back to the launcher");
      runApp(kLauncher);
      return;
    }
    Serial.println("[app] no launcher, using the built-in screen");
    jsvm_start(kFallbackScript, "built-in fallback");
    g_current_app[0] = '\0';
  }

  // Nothing to go back to while the launcher itself is showing.
  showHomeButton(strcmp(g_current_app, kLauncher) != 0);
}

// The way back, drawn once and owned by the firmware. It lives on LVGL's top
// layer rather than the active screen, so jsvm_stop()'s lv_obj_clean() cannot
// delete it and no script can reach it — every app gets an escape hatch it is
// incapable of breaking. Tapping only queues the switch; see requestApp().
static void homeClicked(lv_event_t *) { requestApp(kLauncher); }

static void createHomeButton() {
  g_home_btn = lv_button_create(lv_layer_top());
  lv_obj_set_size(g_home_btn, 34, 34);
  lv_obj_align(g_home_btn, LV_ALIGN_BOTTOM_RIGHT, -3, -3);
  lv_obj_set_style_radius(g_home_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_home_btn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_home_btn, LV_OPA_60, 0);
  lv_obj_set_style_border_color(g_home_btn, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(g_home_btn, 1, 0);
  lv_obj_set_style_border_opa(g_home_btn, LV_OPA_50, 0);
  lv_obj_set_style_shadow_width(g_home_btn, 0, 0);
  lv_obj_add_event_cb(g_home_btn, homeClicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *icon = lv_label_create(g_home_btn);
  lv_label_set_text(icon, LV_SYMBOL_HOME);
  lv_obj_center(icon);
  showHomeButton(false);
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] js-host starting");

  Serial.printf("[boot] board: %s\n", BOARD_NAME);
  Serial.printf("[mem] start                 %u free, %u largest\n",
                heap_caps_get_free_size(JS_HEAP_CAPS),
                heap_caps_get_largest_free_block(JS_HEAP_CAPS));

  if (!board_display_begin()) {
    Serial.println("[boot] gfx->begin() FAILED — halting");
    while (true) delay(1000);
  }
  gfx->fillScreen(RGB565_BLACK);
  Serial.printf("[boot] display %dx%d\n", gfx->width(), gfx->height());

#if BOARD_TOUCH_AXS5106L
  // Capacitive part on I2C; the SPI touch driver brings up its own bus.
  Wire.begin(TOUCH_PIN_SDA, TOUCH_PIN_SCL);
  Wire.setClock(400000);
#endif
  Serial.printf("[boot] touch %s\n", touch_begin() ? "ok" : "not detected");

  // Radio up so wifi.scan() works even before anything is configured. Joining
  // a saved network waits until after LVGL exists, further down: the binding
  // supervises reconnection with an lv_timer, which cannot be created yet.
  WiFi.mode(WIFI_STA);
  Serial.printf("[mem] after WiFi.mode         %u free, %u largest\n",
                heap_caps_get_free_size(JS_HEAP_CAPS),
                heap_caps_get_largest_free_block(JS_HEAP_CAPS));

  lv_init();
  lv_tick_set_cb(lv_tick_cb);

  lv_buf_a = static_cast<uint8_t *>(
      heap_caps_malloc(kBufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
#if BOARD_DOUBLE_BUFFER
  lv_buf_b = static_cast<uint8_t *>(
      heap_caps_malloc(kBufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
#endif
  if (lv_buf_a == nullptr) {
    Serial.println("[boot] draw buffer alloc FAILED — halting");
    while (true) delay(1000);
  }

  lv_display_t *display = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(display, lv_flush_cb);
  lv_display_set_buffers(display, lv_buf_a, lv_buf_b, kBufBytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *touch_indev = lv_indev_create();
  lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch_indev, lv_touch_cb);
  Serial.printf("[mem] after LVGL              %u free, %u largest\n",
                heap_caps_get_free_size(JS_HEAP_CAPS),
                heap_caps_get_largest_free_block(JS_HEAP_CAPS));

  // Storage is mounted once and kept, which is what lets scripts have a real
  // filesystem. The tradeoff is hot-swapping: changing cards now needs a reset.
  board_storage::begin();
  Serial.printf("[boot] storage: sd %s, flash %s\n",
                board_storage::sd() ? "ok" : "none",
                board_storage::flash() ? "ok" : BOARD_HAS_FATFS ? "none" : "n/a");
  Serial.printf("[mem] after storage           %u free, %u largest\n",
                heap_caps_get_free_size(JS_HEAP_CAPS),
                heap_caps_get_largest_free_block(JS_HEAP_CAPS));
  jsvm_set_filesystem(board_storage::sd(), board_storage::flash());

  // Memory left for the JS heap once the display, storage and radio have taken
  // theirs. On a PSRAM-less board this is the number that decides whether the
  // VM can start at all, so it is worth printing every boot rather than only
  // when something fails.
  Serial.printf("[boot] js heap pool: %u bytes free, largest block %u\n",
                heap_caps_get_free_size(JS_HEAP_CAPS),
                heap_caps_get_largest_free_block(JS_HEAP_CAPS));

  createHomeButton();

  // Safe here: LVGL exists, so the binding can create its reconnect timer.
  if (!jsvm_wifi_autoconnect()) {
    Serial.println("[wifi] no saved network — use the Wifi app or wifi.save()");
  }

  runApp(kLauncher);

  fps_window_start = millis();
  setBacklight(80);
  Serial.println("[boot] ready — serial is a JS REPL; 'reload' restarts the app, 'home' opens the launcher");
}

// Same "flash:" convention the loader and the fs bindings use: explicit prefix
// picks flash, otherwise the card when one is fitted and flash when not.
static fs::FS *resolveFs(const char *path, const char **out_path) {
  if (strncmp(path, "flash:", 6) == 0) {
    *out_path = path + 6;
    return board_storage::flash();  // nullptr on boards with no script partition
  }
  *out_path = path;
  fs::FS *sd = board_storage::sd();
  return sd ? sd : board_storage::flash();
}

static bool writeScript(const char *path, const String &text) {
  const char *p;
  fs::FS *dest = resolveFs(path, &p);
  if (!dest) return false;

  // Create the parent directory if it is missing: open(FILE_WRITE) will not, so
  // uploading /apps/x.js to a card with no /apps directory silently failed.
  const char *slash = strrchr(p, '/');
  if (slash && slash != p) {
    String dir(p);
    dir.remove(slash - p);
    if (!dest->exists(dir.c_str())) dest->mkdir(dir.c_str());
  }

  File f = dest->open(p, FILE_WRITE);
  if (!f) return false;
  f.print(text);
  f.close();
  return true;
}

// One line from serial = one JS eval in the running app's context, except for
// host commands:
//   home                       open the launcher
//   reload                     restart the current app from storage
//   ls [dir]                   list a directory (default /)
//   rm <path>                  delete a file
//   app-begin [path]           start receiving a script; defaults to whatever
//     ...lines...              is running now, so the usual edit loop is just
//   app-end                    app-begin / paste / app-end, then it reloads
// REPL state. These live at file scope rather than as function-local statics on
// purpose: a local static with a non-trivial constructor gets a thread-safe
// initialisation guard (__cxa_guard_acquire), and that guard calls abort() on
// this build — a reset loop the moment the REPL is first polled. File-scope
// objects are constructed before main() and need no guard.
static String g_repl_line;
static String g_repl_upload;
static String g_repl_upload_path;
static bool g_repl_uploading = false;

static void pollSerialRepl() {
  String &line = g_repl_line;
  String &upload = g_repl_upload;
  String &upload_path = g_repl_upload_path;
  bool &uploading = g_repl_uploading;
  // Caps so a hostile/broken sender can't grow these Strings until the
  // internal heap dies: REPL lines beyond 4 KB are discarded, uploads beyond
  // 256 KB abort the transfer.
  constexpr size_t kMaxLine = 4 * 1024;
  constexpr size_t kMaxUpload = 256 * 1024;
  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (line.length() < kMaxLine) line += ch;
      continue;
    }
    if (uploading && upload.length() + line.length() > kMaxUpload) {
      uploading = false;
      upload = "";
      Serial.println("[app] upload aborted: exceeds 256 KB cap");
      line = "";
      continue;
    }

    if (uploading) {
      if (line == "app-end") {
        uploading = false;
        if (writeScript(upload_path.c_str(), upload)) {
          Serial.printf("[app] wrote %u bytes to %s\n", upload.length(), upload_path.c_str());
          upload = "";
          requestApp(upload_path.c_str());
        } else {
          Serial.printf("[app] write to %s FAILED\n", upload_path.c_str());
          upload = "";
        }
      } else {
        upload += line;
        upload += '\n';
      }
    } else if (line == "home") {
      requestApp(kLauncher);
    } else if (line == "reload") {
      Serial.println("[app] reload requested over serial");
      requestApp(g_current_app[0] ? g_current_app : kLauncher);
    } else if (line == "ls" || line.startsWith("ls ")) {
      const String dir = (line.length() > 3) ? line.substring(3) : String("/");
      const char *p; fs::FS *f = resolveFs(dir.c_str(), &p);
      File d = f ? f->open(p) : File();
      if (!d || !d.isDirectory()) {
        Serial.printf("[fs] %s is not a directory\n", dir.c_str());
      } else {
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
          Serial.printf("  %-28s %8u%s\n", e.name(), (unsigned)e.size(),
                        e.isDirectory() ? "  <dir>" : "");
          e.close();
        }
      }
      if (d) d.close();
    } else if (line.startsWith("rm ")) {
      const String target = line.substring(3);
      const char *p; fs::FS *f = resolveFs(target.c_str(), &p);
      Serial.printf("[fs] rm %s %s\n", target.c_str(),
                    (f && f->remove(p)) ? "ok" : "FAILED");
    } else if (line == "app-begin" || line.startsWith("app-begin ")) {
      // No path given means "replace what is running", which is the common
      // case while iterating on one app.
      upload_path = (line.length() > 10) ? line.substring(10)
                                         : String(g_current_app[0] ? g_current_app : kLauncher);
      uploading = true;
      upload = "";
      Serial.printf("[app] receiving into %s; finish with app-end\n", upload_path.c_str());
    } else if (line.length()) {
      jsvm_repl_line(line.c_str());
    }
    line = "";
  }
}

void loop() {
  uint32_t idle_ms = lv_timer_handler();
  if (idle_ms == LV_NO_TIMER_READY || idle_ms > 16) idle_ms = 16;

  const uint32_t now = millis();
  if (now - fps_window_start >= 1000) {
    fps_value = flush_count;
    flush_count = 0;
    fps_window_start = now;
  }

  // Long-press BOOT (>= 700 ms) opens the launcher. This is the hardware
  // escape hatch: it works even if an app has covered the home button or the
  // touch panel has stopped responding.
  static uint32_t boot_down_since = 0;
  static bool boot_fired = false;
  if (digitalRead(BOOT_BTN_PIN) == LOW) {
    if (boot_down_since == 0) boot_down_since = now;
    if (!boot_fired && now - boot_down_since >= 700) {
      boot_fired = true;
      Serial.println("[app] launcher requested via BOOT long-press");
      requestApp(kLauncher);
    }
  } else {
    boot_down_since = 0;
    boot_fired = false;
  }

  pollSerialRepl();
  jsvm_pump();  // promise reactions / async continuations

  // Switch apps only here, once everything that could be mid-callback has
  // finished: LVGL event dispatch, the serial REPL, and the promise queue can
  // all ask for a switch, and none of them can survive their own teardown.
  const char *from_js = jsvm_take_pending_launch();
  if (from_js) requestApp(from_js);
  if (g_next_app[0] != '\0') {
    char path[sizeof(g_next_app)];
    strncpy(path, g_next_app, sizeof(path));
    g_next_app[0] = '\0';
    runApp(path);
  }

  delay(idle_ms);
}
