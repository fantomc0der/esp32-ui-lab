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
// app can break: the button the firmware draws in the corner of LVGL's top
// layer, and a long-press of BOOT. Every switch is queued and performed from
// loop(), never from inside a callback (see requestApp).
//
// A missing or throwing app falls back to the launcher; a missing launcher
// falls back to a screen built into the firmware, so the panel is never dead.
//
// Pinning turns the board into a single-app appliance: with a pin set (sys.pin()
// from a script, or `pin` over serial) the boot goes straight to that script and
// the corner stays empty, so nothing on screen hints at a launcher that is no
// longer part of the product. BOOT long-press still reaches it, which is how you
// unpin a board with no serial attached.
//
// That corner is one slot showing at most one control, chosen from where you
// are and what the app needs (see updateCornerButton): the way back out of an
// app, and — for an app that wants a network the board has not got — the way
// to the Wi-Fi setup app, which is the one thing a pinned appliance cannot
// otherwise offer a route to.
//
// The serial port is a JS REPL into the running app plus a few host commands
// (home, reload, pin, unpin, ls, rm, app-begin/app-end) — see pollSerialRepl().
//
// The JS engine and the LVGL bindings are libraries beside this sketch
// (../quickjs-ng, ../lvgl-js-bindings); what stays here is the hardware
// bring-up plus the policy choices: where scripts come from, what triggers a
// reload, and what the serial port does. Build with lang-js/flash.ps1.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FFat.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>

#include <js_bindings.h>

#include "axs5106l_touch.h"
#include "board_pins.h"
#include "jd9853_panel.h"
#include "js_fallback.h"

// QuickJS recurses on the C stack; the default 8 KB loopTask stack is too
// small. The VM's own limit (js_bindings.cpp) stays well under this.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

// ---------------------------------------------------------------- display stack
// Identical to lang-c/app — see that sketch and docs/lang-c/ for
// the reasoning behind every constant here.

Arduino_DataBus *bus =
    new Arduino_ESP32SPI(LCD_PIN_DC, LCD_PIN_CS, LCD_PIN_SCK, LCD_PIN_MOSI);

Arduino_GFX *gfx = new Arduino_ST7789(
    bus, GFX_NOT_DEFINED /* RST handled manually */, 0, true /* IPS */,
    LCD_NATIVE_W, LCD_NATIVE_H, LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET,
    LCD_ROW_OFFSET);

// RGB565 = 2 bytes/px on the wire; never size buffers with sizeof(lv_color_t)
// (3 in LVGL 9).
static constexpr uint32_t kBytesPerPx = 2;
static constexpr uint32_t kBufBytes = SCREEN_W * (SCREEN_H / 8) * kBytesPerPx;
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

// This board halves the battery rail before the ADC. GPIO12 is an ADC2
// channel, and the WiFi driver arbitrates ADC2, so a 0 reading means
// "unavailable" rather than "flat"; NAN reaches scripts as null.
float jsvm_host_battery() {
  const uint32_t mv = analogReadMilliVolts(BAT_PIN);
  if (mv == 0) return NAN;
  return (mv * 2.0f) / 1000.0f;
}

// ---------------------------------------------------------------- script loading

// Reads a whole file into a PSRAM buffer (caller frees). nullptr on any miss.
static char *readAll(fs::FS &fs, const char *path) {
  File f = fs.open(path, FILE_READ);
  if (!f || f.isDirectory()) return nullptr;
  const size_t n = f.size();
  char *buf = static_cast<char *>(heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM));
  if (!buf) { f.close(); return nullptr; }
  const size_t got = f.read(reinterpret_cast<uint8_t *>(buf), n);
  f.close();
  buf[got] = '\0';
  return buf;
}

// The launcher: the script that lists the others. Everything else is reached
// from it, and it is where a failed app lands you.
static const char *kLauncher = "/app.js";

// The network setup app. Which script that is, is host policy in the same way
// the launcher is: the binding layer reports that the running app wants a
// network it hasn't got, and this is where the firmware sends you to fix it.
static const char *kWifiApp = "/apps/wifi.js";

static bool g_sd_ok = false;
static bool g_flash_ok = false;
static char g_current_app[128] = "";
static char g_next_app[128] = "";

// Queues an app switch. Every path that changes apps goes through here — a
// script's sys.launch(), the corner button, the BOOT button — so the actual
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
  if (strncmp(path, "flash:", 6) == 0) {
    return g_flash_ok ? readAll(FFat, path + 6) : nullptr;
  }
  char *src = g_sd_ok ? readAll(SD_MMC, path) : nullptr;
  if (!src && g_flash_ok) src = readAll(FFat, path);
  return src;
}

// ---------------------------------------------------------------- corner button

// One slot in the bottom-right corner, and at most one control in it. What
// belongs there is derived from where you are and what the running app needs,
// never set by whoever caused the change, because a script can pin itself or
// discover it has no network at any moment.
//
// The enum stays out of every function signature on purpose: the Arduino
// preprocessor generates a prototype for each function in a .ino and inserts
// them all above the sketch's own code, so a signature naming a type declared
// here does not compile.
enum CornerButton {
  kCornerNone,
  kCornerHome,  // to the launcher
  kCornerBack,  // to the pinned app, from a detour away from it
  kCornerWifi,  // to network setup
};

static lv_obj_t *g_corner_btn = nullptr;
static lv_obj_t *g_corner_icon = nullptr;
static CornerButton g_corner = kCornerNone;  // createCornerButton() starts it hidden

// Where leaving the current app goes. Normally the launcher; on a pinned board
// the pinned app, since a pin says the launcher is not part of the product and
// the appliance is what you expect to come back to.
static const char *cornerBackTarget() {
  const char *pinned = jsvm_pinned_app();
  return pinned ? pinned : kLauncher;
}

// Cheap enough to call every loop(), which is what keeps the corner right
// without anything having to remember to update it.
static void updateCornerButton() {
  if (!g_corner_btn) return;

  CornerButton want;
  if (jsvm_network_setup_needed() && strcmp(g_current_app, kWifiApp) != 0) {
    // Setup outranks navigation. An app with no network to talk to is stuck at
    // something the user can fix in two taps, and on a pinned board this is the
    // only visible route to fixing it — the corner would otherwise be empty and
    // a BOOT long-press is not something anyone discovers. Not offered inside
    // the Wi-Fi app itself, which is already the destination.
    want = kCornerWifi;
  } else if (strcmp(g_current_app, cornerBackTarget()) == 0) {
    // Nothing to offer while you are already where the button would take you:
    // an escape hatch you are not meant to use is a stray control on the UI.
    want = kCornerNone;
  } else {
    want = jsvm_pinned_app() ? kCornerBack : kCornerHome;
  }

  if (want == g_corner) return;
  g_corner = want;
  if (want == kCornerNone) {
    lv_obj_add_flag(g_corner_btn, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(g_corner_icon, want == kCornerWifi ? LV_SYMBOL_WIFI
                                 : want == kCornerBack ? LV_SYMBOL_LEFT
                                                       : LV_SYMBOL_HOME);
  lv_obj_remove_flag(g_corner_btn, LV_OBJ_FLAG_HIDDEN);
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

  updateCornerButton();
}

// Tapping only queues the switch; see requestApp(). Reading g_corner rather
// than recomputing keeps the tap honest: you get the button you saw.
static void cornerClicked(lv_event_t *) {
  requestApp(g_corner == kCornerWifi ? kWifiApp : cornerBackTarget());
}

// Drawn once and owned by the firmware. It lives on LVGL's top layer rather
// than the active screen, so jsvm_stop()'s lv_obj_clean() cannot delete it and
// no script can reach it — every app gets an escape hatch it is incapable of
// breaking.
static void createCornerButton() {
  g_corner_btn = lv_button_create(lv_layer_top());
  lv_obj_set_size(g_corner_btn, 34, 34);
  lv_obj_align(g_corner_btn, LV_ALIGN_BOTTOM_RIGHT, -3, -3);
  lv_obj_set_style_radius(g_corner_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_corner_btn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_corner_btn, LV_OPA_60, 0);
  lv_obj_set_style_border_color(g_corner_btn, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(g_corner_btn, 1, 0);
  lv_obj_set_style_border_opa(g_corner_btn, LV_OPA_50, 0);
  lv_obj_set_style_shadow_width(g_corner_btn, 0, 0);
  lv_obj_add_event_cb(g_corner_btn, cornerClicked, LV_EVENT_CLICKED, nullptr);

  g_corner_icon = lv_label_create(g_corner_btn);
  lv_label_set_text(g_corner_icon, LV_SYMBOL_HOME);
  lv_obj_center(g_corner_icon);
  lv_obj_add_flag(g_corner_btn, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] js-host starting");

  pinMode(LCD_PIN_BL, OUTPUT);
  analogWrite(LCD_PIN_BL, 0);  // no garbage RAM on the panel during init

  pinMode(LCD_PIN_RST, OUTPUT);
  digitalWrite(LCD_PIN_RST, LOW);
  delay(20);
  digitalWrite(LCD_PIN_RST, HIGH);
  delay(150);

  if (!gfx->begin(40000000L)) {
    Serial.println("[boot] gfx->begin() FAILED — halting");
    while (true) delay(1000);
  }
  jd9853_init(bus);
  gfx->setRotation(LCD_ROTATION);
  gfx->fillScreen(RGB565_BLACK);
  Serial.printf("[boot] display %dx%d\n", gfx->width(), gfx->height());

  Wire.begin(TOUCH_PIN_SDA, TOUCH_PIN_SCL);
  Wire.setClock(400000);
  touch_begin();

  // Radio up so wifi.scan() works even before anything is configured. Joining
  // a saved network waits until after LVGL exists, further down: the binding
  // supervises reconnection with an lv_timer, which cannot be created yet.
  WiFi.mode(WIFI_STA);

  lv_init();
  lv_tick_set_cb(lv_tick_cb);

  lv_buf_a = static_cast<uint8_t *>(
      heap_caps_malloc(kBufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  lv_buf_b = static_cast<uint8_t *>(
      heap_caps_malloc(kBufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
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

  // Storage is mounted once and kept, which is what lets scripts have a real
  // filesystem. The tradeoff is hot-swapping: changing cards now needs a reset.
  SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0, SD_PIN_D1, SD_PIN_D2, SD_PIN_D3);
  g_sd_ok = SD_MMC.begin("/sdcard", false /* 4-bit */) && SD_MMC.cardType() != CARD_NONE;
  g_flash_ok = FFat.begin(true /* format on first use */);
  Serial.printf("[boot] storage: sd %s, flash %s\n",
                g_sd_ok ? "ok" : "none", g_flash_ok ? "ok" : "none");
  jsvm_set_filesystem(g_sd_ok ? &SD_MMC : nullptr, g_flash_ok ? &FFat : nullptr);

  createCornerButton();

  // Safe here: LVGL exists, so the binding can create its reconnect timer.
  if (!jsvm_wifi_autoconnect()) {
    Serial.println("[wifi] no saved network — use the Wifi app or wifi.save()");
  }

  // A pinned app takes the place of the launcher entirely. If it is missing or
  // throws, runApp() still lands on the launcher, so a bad pin costs you the
  // appliance behaviour but never the board.
  const char *pinned = jsvm_pinned_app();
  if (pinned) Serial.printf("[app] %s is pinned — skipping the launcher\n", pinned);
  runApp(pinned ? pinned : kLauncher);

  fps_window_start = millis();
  setBacklight(80);
  Serial.println("[boot] ready — serial is a JS REPL; 'reload' restarts the app, 'home' opens the launcher");
}

// Same "flash:" convention the loader and the fs bindings use: explicit prefix
// picks flash, otherwise the card when one is fitted and flash when not.
static fs::FS *resolveFs(const char *path, const char **out_path) {
  if (strncmp(path, "flash:", 6) == 0) {
    *out_path = path + 6;
    return g_flash_ok ? static_cast<fs::FS *>(&FFat) : nullptr;
  }
  *out_path = path;
  if (g_sd_ok) return static_cast<fs::FS *>(&SD_MMC);
  return g_flash_ok ? static_cast<fs::FS *>(&FFat) : nullptr;
}

static bool writeScript(const char *path, const String &text) {
  const char *p;
  fs::FS *dest = resolveFs(path, &p);
  if (!dest) return false;

  if (!*p) return false;  // "flash:" with nothing after it leaves p empty

  // open(FILE_WRITE) will not create intervening directories, so uploading
  // /apps/x.js to a freshly formatted card reported only "write FAILED" — which
  // reads as a broken card rather than a missing folder. Walk the path and create
  // each level: mkdir() itself is not recursive either, so creating only the
  // immediate parent would leave a nested path failing the same silent way.
  // Starting at p + 1 skips a leading slash, which is the root and always exists.
  for (const char *slash = strchr(p + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
    String dir(p);
    dir.remove(slash - p);
    if (!dest->exists(dir.c_str()) && !dest->mkdir(dir.c_str())) {
      // Say which level failed. Otherwise this lands as the same undifferentiated
      // "write FAILED" the walk exists to eliminate.
      Serial.printf("[fs] mkdir %s FAILED\n", dir.c_str());
      return false;
    }
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
//   pin [path]                 boot straight into this app from now on,
//                              defaulting to whatever is running
//   unpin                      go back to booting the launcher
//   ls [dir]                   list a directory (default /)
//   rm <path>                  delete a file
//   app-begin [path]           start receiving a script; defaults to whatever
//     ...lines...              is running now, so the usual edit loop is just
//   app-end                    app-begin / paste / app-end, then it reloads
static void pollSerialRepl() {
  static String line;
  static String upload;
  static String upload_path;
  static bool uploading = false;
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
    } else if (line == "pin" || line.startsWith("pin ")) {
      // Bare `pin` means "this one", the case you are in after tapping an app.
      const String target = (line.length() > 4) ? line.substring(4) : String(g_current_app);
      if (target.isEmpty() || target == kLauncher) {
        Serial.println("[app] pin <path>, or run the app you want pinned first");
      } else if (!jsvm_set_pinned_app(target.c_str())) {
        Serial.println("[app] pin FAILED (NVS unavailable)");
      }
    } else if (line == "unpin") {
      jsvm_set_pinned_app(nullptr);
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
  // escape hatch: it works even if an app has covered the home button, a pin
  // has removed it, or the touch panel has stopped responding.
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
  // A script may have pinned itself, or found it has no network, since the
  // last pass.
  updateCornerButton();

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
