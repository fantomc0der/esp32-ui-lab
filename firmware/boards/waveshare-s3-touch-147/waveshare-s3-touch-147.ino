// waveshare-s3-touch-147 — the board sketch for the Waveshare
// ESP32-S3-Touch-LCD-1.47.
//
// The firmware is C; the UI is not. This sketch brings up the display, touch,
// LVGL and storage, then hands the screen to JavaScript executed by QuickJS-ng.
//
// Everything past the hardware is the libraries beside this sketch
// (../quickjs-ng, ../lvgl-js-bindings): which script boots, what the corner
// button offers, what the serial port does, and how a reload is performed are
// all board-independent policy owned by jsvm_app_*(). What is left here is this
// board: its pinout, its panel, its touch controller, and the three host hooks.
//
// Every constant below is explained in docs/hardware/. Build with flash.ps1 at
// the repo root.

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

// QuickJS recurses on the C stack; the default 8 KB loopTask stack is too
// small. The VM's own limit (jsvm_core.cpp) stays well under this.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

// ---------------------------------------------------------------- display stack

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

// ---------------------------------------------------------------- setup / loop

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] waveshare-s3-touch-147 starting");

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
  // a saved network waits until jsvm_app_begin(), which runs after LVGL exists:
  // the binding supervises reconnection with an lv_timer.
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
  const bool sd_ok = SD_MMC.begin("/sdcard", false /* 4-bit */) && SD_MMC.cardType() != CARD_NONE;
  const bool flash_ok = FFat.begin(true /* format on first use */);

  JsvmAppConfig cfg;
  cfg.sd = sd_ok ? static_cast<fs::FS *>(&SD_MMC) : nullptr;
  cfg.flash = flash_ok ? static_cast<fs::FS *>(&FFat) : nullptr;
  cfg.launcher = "/app.js";
  cfg.wifi_app = "/apps/wifi.js";
  cfg.home_button_pin = BOOT_BTN_PIN;
  jsvm_app_begin(cfg);

  fps_window_start = millis();
  setBacklight(80);
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

  jsvm_app_service();

  delay(idle_ms);
}
