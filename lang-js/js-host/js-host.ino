// js-host — JavaScript-scripted LVGL runtime for the Waveshare
// ESP32-S3-Touch-LCD-1.47.
//
// The firmware is C; the UI is not. At boot this sketch brings up the display,
// touch, and LVGL exactly like the proven C demo (lang-c/app),
// then hands control to a JavaScript file executed by QuickJS-ng:
//
//   1. /app.js on the microSD card        (edit on a PC, move the card)
//   2. /app.js on the 9.9 MB FATFS flash partition
//   3. a built-in fallback script          (says "no app.js found" on screen)
//
// Long-press BOOT (>= 700 ms) to reload: the JS world is torn down and app.js
// is re-read from storage — the edit loop needs no compiler and no reflash.
// The serial port is a JS REPL into the running app (one line = one eval),
// and "reload" typed on serial triggers the same reload as the button.
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

// Mount SD fresh on every call so a card swapped while powered is seen.
static char *loadFromSd() {
  SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0, SD_PIN_D1, SD_PIN_D2, SD_PIN_D3);
  if (!SD_MMC.begin("/sdcard", false /* 4-bit */)) return nullptr;
  char *src = (SD_MMC.cardType() != CARD_NONE) ? readAll(SD_MMC, "/app.js") : nullptr;
  SD_MMC.end();
  return src;
}

static char *loadFromFlash() {
  // format-on-fail: first boot leaves the 9.9 MB FATFS partition unformatted.
  if (!FFat.begin(true)) return nullptr;
  char *src = readAll(FFat, "/app.js");
  FFat.end();
  return src;
}

// Loads app.js (SD, then FATFS, then built-in) and starts the VM. A script
// that throws at boot is torn down and replaced by the fallback screen.
static void runApp() {
  jsvm_stop();

  char *src = loadFromSd();
  const char *origin = "sd:/app.js";
  if (!src) { src = loadFromFlash(); origin = "ffat:/app.js"; }
  if (!src) { origin = "built-in fallback"; }
  Serial.printf("[app] running %s\n", origin);

  bool ok;
  if (src) {
    ok = jsvm_start(src, origin);
    heap_caps_free(src);
  } else {
    ok = jsvm_start(kFallbackScript, origin);
  }

  if (!ok && src) {
    Serial.println("[app] script failed, showing fallback");
    jsvm_stop();
    jsvm_start(kFallbackScript, "built-in fallback");
  }
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

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

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

  runApp();

  fps_window_start = millis();
  setBacklight(80);
  Serial.println("[boot] ready — serial is a JS REPL; 'reload' = reload app.js");
}

// One line from serial = one JS eval in the running app's context, except for
// host commands:
//   reload                     tear down and re-read app.js
//   app-begin ... app-end      capture the lines in between into ffat:/app.js
//                              (script upload without touching the SD card),
//                              then reload
//   app-clear                  delete ffat:/app.js and reload
static void pollSerialRepl() {
  static String line;
  static String upload;
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
        if (FFat.begin(true)) {
          File f = FFat.open("/app.js", FILE_WRITE);
          if (f) {
            f.print(upload);
            f.close();
            Serial.printf("[app] wrote %u bytes to ffat:/app.js\n", upload.length());
          } else {
            Serial.println("[app] ffat:/app.js open FAILED");
          }
          FFat.end();
        } else {
          Serial.println("[app] FFat mount FAILED");
        }
        upload = "";
        runApp();
      } else {
        upload += line;
        upload += '\n';
      }
    } else if (line == "reload") {
      Serial.println("[app] reload requested over serial");
      runApp();
    } else if (line == "app-begin") {
      uploading = true;
      upload = "";
      Serial.println("[app] receiving script; finish with app-end");
    } else if (line == "app-clear") {
      if (FFat.begin(true)) {
        FFat.remove("/app.js");
        FFat.end();
        Serial.println("[app] ffat:/app.js removed");
      }
      runApp();
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

  // Long-press BOOT (>= 700 ms) = reload. Fires once per press.
  static uint32_t boot_down_since = 0;
  static bool boot_fired = false;
  if (digitalRead(BOOT_BTN_PIN) == LOW) {
    if (boot_down_since == 0) boot_down_since = now;
    if (!boot_fired && now - boot_down_since >= 700) {
      boot_fired = true;
      Serial.println("[app] reload requested via BOOT long-press");
      runApp();
    }
  } else {
    boot_down_since = 0;
    boot_fired = false;
  }

  pollSerialRepl();
  jsvm_pump();  // promise reactions / async continuations
  delay(idle_ms);
}
