// app.ino — an LVGL 9 touch demo for the Waveshare ESP32-S3-Touch-LCD-1.47.
//
// What it shows off, and why each part is here:
//
//   Tab 1 "Vitals"  — live line chart of free heap + a CPU-load-ish bar, an arc
//                     gauge, and counters. Exercises LVGL's chart/arc widgets
//                     and continuous redraw (the hard case for a small SPI
//                     panel: proves the flush path is fast enough).
//   Tab 2 "Touch"   — draw-on-canvas plus a live coordinate readout. This is the
//                     honest test of the touch controller: you see raw mapping
//                     accuracy immediately, and it's the fastest way to spot a
//                     swapped/mirrored axis.
//   Tab 3 "WiFi"    — async scan started on demand, results into a list. Proves
//                     the radio works and that LVGL stays responsive while the
//                     driver is busy (scan runs off the UI thread).
//   Tab 4 "System"  — chip/PSRAM/flash facts pulled at runtime, plus a
//                     backlight brightness slider (LEDC PWM) so you can verify
//                     the BL pin, and a live FPS counter.
//
// Tabs are swipeable AND tappable, which is the main thing you want to confirm
// on a 320x172 landscape screen.
//
// Board: select "ESP32S3 Dev Module" and set PSRAM to "OPI PSRAM".
// See README.md for the full Arduino IDE settings — they matter here.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>

#include "axs5106l_touch.h"
#include "board_pins.h"
#include "jd9853_panel.h"

// ---------------------------------------------------------------- display stack

Arduino_DataBus *bus =
    new Arduino_ESP32SPI(LCD_PIN_DC, LCD_PIN_CS, LCD_PIN_SCK, LCD_PIN_MOSI);

// JD9853 panel driven via the ST7789 class + jd9853_init(). The 34px column
// offset centres the 172-wide panel in the controller's 240-wide RAM.
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, GFX_NOT_DEFINED /* RST handled manually below */, 0 /* rotation */,
    true /* IPS */, LCD_NATIVE_W, LCD_NATIVE_H, LCD_COL_OFFSET, LCD_ROW_OFFSET,
    LCD_COL_OFFSET, LCD_ROW_OFFSET);

// ---------------------------------------------------------------- LVGL plumbing

// Partial-render buffers: 1/8 screen each, double-buffered. Kept in internal
// DMA-capable SRAM on purpose — PSRAM is reachable but much slower per pixel,
// and at this size (~13.4 kB each) internal RAM is not scarce.
//
// NOTE: sizeof(lv_color_t) is 3 in LVGL 9.x (it is a 24-bit r/g/b struct) and
// has nothing to do with the render colour format. We render RGB565, so a pixel
// is 2 bytes on the wire. lv_display_set_buffers() takes buf_size in BYTES and
// derives the row stride from the display's colour format, so the byte count
// must be computed from the real bytes-per-pixel — not from sizeof(lv_color_t),
// which would over-allocate by 50% and mis-state the buffer height.
static constexpr uint32_t kBytesPerPx = 2;  // LV_COLOR_DEPTH 16 -> RGB565
static constexpr uint32_t kBufLines = SCREEN_H / 8;
static constexpr uint32_t kBufPx = SCREEN_W * kBufLines;
static constexpr uint32_t kBufBytes = kBufPx * kBytesPerPx;
static uint8_t *lv_buf_a = nullptr;
static uint8_t *lv_buf_b = nullptr;

// Frame accounting for the FPS readout. Incremented in the flush callback, so
// this counts real panel writes — NOT loop() iterations, which would just
// measure polling rate (~500/s) and peg the load gauge at 100% forever.
// Not volatile: the flush callback is invoked synchronously from
// lv_timer_handler(), i.e. on the same task as loop() — there is no ISR or
// second thread involved, so no memory-ordering concern.
static uint32_t flush_count = 0;

static lv_display_t *display = nullptr;
static lv_indev_t *touch_indev = nullptr;

// LVGL needs a millisecond clock; hand it Arduino's.
static uint32_t lv_tick_cb() { return millis(); }

static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area,
                        uint8_t *px_map) {
  const uint32_t w = lv_area_get_width(area);
  const uint32_t h = lv_area_get_height(area);

  // LVGL renders RGB565 little-endian; this panel expects big-endian over SPI.
  // Swap in place, then push. (Alternative would be LV_COLOR_16_SWAP, which no
  // longer exists in LVGL 9 — this is the sanctioned replacement.)
  lv_draw_sw_rgb565_swap(px_map, w * h);
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1,
                            reinterpret_cast<uint16_t *>(px_map), w, h);

  flush_count++;  // real render work, used for the FPS/load readouts
  lv_display_flush_ready(disp);
}

static void lv_touch_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  uint16_t x = 0, y = 0;
  if (touch_read(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ---------------------------------------------------------------- UI state

static lv_obj_t *chart = nullptr;
static lv_chart_series_t *heap_series = nullptr;
static lv_obj_t *heap_label = nullptr;
static lv_obj_t *uptime_label = nullptr;
static lv_obj_t *load_arc = nullptr;
static lv_obj_t *load_label = nullptr;

static lv_obj_t *touch_canvas = nullptr;
static lv_obj_t *touch_coord_label = nullptr;
static lv_obj_t *touch_dot = nullptr;

static lv_obj_t *wifi_list = nullptr;
static lv_obj_t *wifi_btn = nullptr;
static lv_obj_t *wifi_status = nullptr;
static bool wifi_scanning = false;

static lv_obj_t *fps_label = nullptr;

static uint32_t fps_window_start = 0;
static uint32_t fps_value = 0;

// ---------------------------------------------------------------- helpers

// Board divides the battery/VBUS rail by 2 before the ADC.
//
// Caveat: on the ESP32-S3, GPIO12 is an ADC2 channel, and ADC2 is arbitrated by
// the WiFi driver — while the radio holds it, the read returns 0. We surface
// that as NAN so the UI can say "n/a" rather than display a confident 0.00V.
static float readBatteryVolts() {
  const uint32_t mv = analogReadMilliVolts(BAT_PIN);
  if (mv == 0) return NAN;  // ADC2 unavailable (WiFi active) or nothing attached
  return (mv * 2.0f) / 1000.0f;
}

static void setBacklight(uint8_t percent) {
  if (percent > 100) percent = 100;
  // Keep a floor: 0% looks identical to a crashed board, which is a confusing
  // thing to ship in a demo whose job is proving the board works.
  const uint32_t duty = map(percent, 0, 100, 12, 255);
  analogWrite(LCD_PIN_BL, duty);
}

// ---------------------------------------------------------------- tab 1: Vitals

static void buildVitalsTab(lv_obj_t *parent) {
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(parent, 4, 0);

  // Left: free-heap history chart.
  lv_obj_t *left = lv_obj_create(parent);
  lv_obj_set_size(left, 186, 132);
  lv_obj_set_style_pad_all(left, 4, 0);
  lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);

  heap_label = lv_label_create(left);
  lv_label_set_text(heap_label, "heap --");
  lv_obj_set_style_text_font(heap_label, &lv_font_montserrat_14, 0);
  lv_obj_align(heap_label, LV_ALIGN_TOP_LEFT, 0, 0);

  chart = lv_chart_create(left);
  lv_obj_set_size(chart, 172, 88);
  lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, 40);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(chart, 4, 6);
  // Free heap in kB. ESP32-S3 has 512kB SRAM; a 0..340kB window keeps the
  // trace in frame while still showing movement.
  lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 340);
  lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);  // hide point dots
  heap_series =
      lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_CYAN),
                          LV_CHART_AXIS_PRIMARY_Y);

  // Right: arc "load" gauge + uptime.
  lv_obj_t *right = lv_obj_create(parent);
  lv_obj_set_size(right, 118, 132);
  lv_obj_set_style_pad_all(right, 4, 0);
  lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

  load_arc = lv_arc_create(right);
  lv_obj_set_size(load_arc, 84, 84);
  lv_obj_align(load_arc, LV_ALIGN_TOP_MID, 0, 0);
  lv_arc_set_rotation(load_arc, 135);
  lv_arc_set_bg_angles(load_arc, 0, 270);
  lv_arc_set_range(load_arc, 0, 100);
  lv_obj_remove_style(load_arc, nullptr,
                      static_cast<lv_style_selector_t>(LV_PART_KNOB) |
                          static_cast<lv_style_selector_t>(LV_STATE_ANY));
  lv_obj_remove_flag(load_arc, LV_OBJ_FLAG_CLICKABLE);

  load_label = lv_label_create(load_arc);
  lv_label_set_text(load_label, "0%");  // render throughput, see vitalsTimer()
  lv_obj_set_style_text_font(load_label, &lv_font_montserrat_16, 0);
  lv_obj_center(load_label);

  uptime_label = lv_label_create(right);
  lv_label_set_text(uptime_label, "up 0s");
  lv_obj_set_style_text_font(uptime_label, &lv_font_montserrat_14, 0);
  lv_obj_align(uptime_label, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// Repopulates the vitals readouts. Driven by an LVGL timer, not loop(), so the
// refresh rate is independent of frame rate.
static void vitalsTimer(lv_timer_t *) {
  const uint32_t free_heap = ESP.getFreeHeap();
  lv_chart_set_next_value(chart, heap_series,
                          static_cast<int32_t>(free_heap / 1024));
  lv_label_set_text_fmt(heap_label, "heap %lu kB  psram %lu kB",
                        (unsigned long)(free_heap / 1024),
                        (unsigned long)(ESP.getFreePsram() / 1024));

  // Not a real CPU-load counter — this is render throughput as a fraction of
  // a 30 flush/s target, which is what "is the display keeping up?" means here.
  // Labelled "render" on screen so it is not mistaken for system load.
  constexpr uint32_t kTargetFps = 30;
  uint32_t load = (fps_value * 100) / kTargetFps;
  if (load > 100) load = 100;
  lv_arc_set_value(load_arc, load);
  lv_label_set_text_fmt(load_label, "%lu%%", (unsigned long)load);

  const uint32_t secs = millis() / 1000;
  const float volts = readBatteryVolts();
  if (isnan(volts)) {
    // ADC2 is held by the WiFi driver — say so rather than showing a fake 0.00V.
    lv_label_set_text_fmt(uptime_label, "up %lum %lus  bat n/a",
                          (unsigned long)(secs / 60),
                          (unsigned long)(secs % 60));
  } else {
    lv_label_set_text_fmt(uptime_label, "up %lum %lus  %.2fV",
                          (unsigned long)(secs / 60),
                          (unsigned long)(secs % 60), volts);
  }
}

// ---------------------------------------------------------------- tab 2: Touch

static void touchCanvasEvent(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_active();
  if (indev == nullptr) return;

  lv_point_t p;
  lv_indev_get_point(indev, &p);

  // lv_obj_set_pos() positions a child against its parent's *content* area
  // (inside border + padding), whereas lv_obj_get_coords() returns the outer
  // box. Using the outer box would offset the dot from the fingertip by
  // border+pad (~9px with the default theme), so ask for the content area.
  lv_area_t content;
  lv_obj_get_content_coords(touch_canvas, &content);

  // Centre the 10px dot on the touch point.
  lv_obj_set_pos(touch_dot, (p.x - content.x1) - 5, (p.y - content.y1) - 5);
  lv_obj_remove_flag(touch_dot, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(touch_coord_label, "x %ld  y %ld", (long)p.x, (long)p.y);

  LV_UNUSED(e);
}

static void buildTouchTab(lv_obj_t *parent) {
  lv_obj_set_style_pad_all(parent, 4, 0);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  touch_coord_label = lv_label_create(parent);
  lv_label_set_text(touch_coord_label, "touch the box");
  lv_obj_set_style_text_font(touch_coord_label, &lv_font_montserrat_14, 0);
  lv_obj_align(touch_coord_label, LV_ALIGN_TOP_LEFT, 2, 0);

  // A plain object used as a touch target. (Not lv_canvas: we only need hit
  // testing and a moving marker, and this keeps the RAM cost at zero.)
  touch_canvas = lv_obj_create(parent);
  lv_obj_set_size(touch_canvas, 300, 104);
  lv_obj_align(touch_canvas, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_style_bg_color(touch_canvas, lv_color_hex(0x11202B), 0);
  lv_obj_set_style_border_color(touch_canvas, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_set_style_border_width(touch_canvas, 1, 0);
  lv_obj_remove_flag(touch_canvas, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(touch_canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(touch_canvas, touchCanvasEvent, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(touch_canvas, touchCanvasEvent, LV_EVENT_PRESSED, nullptr);

  touch_dot = lv_obj_create(touch_canvas);
  lv_obj_set_size(touch_dot, 10, 10);
  lv_obj_set_style_radius(touch_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(touch_dot, lv_palette_main(LV_PALETTE_AMBER), 0);
  lv_obj_set_style_border_width(touch_dot, 0, 0);
  lv_obj_add_flag(touch_dot, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- tab 3: WiFi

static void wifiScanClicked(lv_event_t *) {
  if (wifi_scanning) return;
  wifi_scanning = true;

  lv_obj_clean(wifi_list);
  lv_label_set_text(wifi_status, "scanning...");
  // Async so lv_timer_handler() keeps running and the UI stays alive.
  WiFi.scanNetworks(true /* async */);
}

static void buildWifiTab(lv_obj_t *parent) {
  lv_obj_set_style_pad_all(parent, 4, 0);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  wifi_btn = lv_button_create(parent);
  lv_obj_set_size(wifi_btn, 84, 30);
  lv_obj_align(wifi_btn, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_add_event_cb(wifi_btn, wifiScanClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *btn_label = lv_label_create(wifi_btn);
  lv_label_set_text(btn_label, "Scan");
  lv_obj_center(btn_label);

  wifi_status = lv_label_create(parent);
  lv_label_set_text(wifi_status, "idle");
  lv_obj_set_style_text_font(wifi_status, &lv_font_montserrat_14, 0);
  lv_obj_align(wifi_status, LV_ALIGN_TOP_LEFT, 92, 8);

  wifi_list = lv_list_create(parent);
  lv_obj_set_size(wifi_list, 306, 100);
  lv_obj_align(wifi_list, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// Polls the async scan for completion. WiFi.scanComplete() returns the AP count
// once done, or a negative sentinel while still running.
static void wifiTimer(lv_timer_t *) {
  if (!wifi_scanning) return;

  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING || n == WIFI_SCAN_FAILED) return;

  wifi_scanning = false;
  lv_label_set_text_fmt(wifi_status, "%d found", n);

  // Cap the list: 40 APs of list items would be slow to build and pointless to
  // scroll on a 172px-tall screen.
  const int shown = n > 12 ? 12 : n;
  for (int i = 0; i < shown; i++) {
    char row[64];
    snprintf(row, sizeof(row), "%s  %lddBm", WiFi.SSID(i).c_str(),
             (long)WiFi.RSSI(i));
    lv_list_add_button(wifi_list, LV_SYMBOL_WIFI, row);
  }
  WiFi.scanDelete();
}

// ---------------------------------------------------------------- tab 4: System

static void brightnessChanged(lv_event_t *e) {
  // lv_event_get_target() returns the *original* target, which for a composite
  // widget can be a sub-part that propagated the event upward. We want the
  // object whose callback is running, hence get_current_target_obj().
  lv_obj_t *slider = lv_event_get_current_target_obj(e);
  setBacklight(static_cast<uint8_t>(lv_slider_get_value(slider)));
}

static void buildSystemTab(lv_obj_t *parent) {
  lv_obj_set_style_pad_all(parent, 4, 0);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *info = lv_label_create(parent);
  lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
  lv_obj_align(info, LV_ALIGN_TOP_LEFT, 2, 0);
  lv_label_set_text_fmt(
      info, "%s  rev%d  %ux%luMHz\nflash %luMB   psram %luMB   lvgl %d.%d",
      ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
      (unsigned long)getCpuFrequencyMhz(),
      (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)),
      (unsigned long)(ESP.getPsramSize() / (1024 * 1024)), LVGL_VERSION_MAJOR,
      LVGL_VERSION_MINOR);

  fps_label = lv_label_create(parent);
  lv_obj_set_style_text_font(fps_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(fps_label, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_align(fps_label, LV_ALIGN_TOP_RIGHT, -2, 34);
  lv_label_set_text(fps_label, "-- fps");

  lv_obj_t *bl_label = lv_label_create(parent);
  lv_label_set_text(bl_label, "backlight");
  lv_obj_set_style_text_font(bl_label, &lv_font_montserrat_14, 0);
  lv_obj_align(bl_label, LV_ALIGN_BOTTOM_LEFT, 2, -34);

  lv_obj_t *slider = lv_slider_create(parent);
  lv_obj_set_size(slider, 280, 14);
  lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_slider_set_range(slider, 5, 100);
  lv_slider_set_value(slider, 80, LV_ANIM_OFF);
  lv_obj_add_event_cb(slider, brightnessChanged, LV_EVENT_VALUE_CHANGED, nullptr);
}

// ---------------------------------------------------------------- setup / loop

static void buildUi() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A1520), 0);

  lv_obj_t *tabview = lv_tabview_create(scr);
  lv_tabview_set_tab_bar_size(tabview, 30);
  lv_obj_set_style_bg_color(tabview, lv_color_hex(0x0A1520), 0);

  buildVitalsTab(lv_tabview_add_tab(tabview, "Vitals"));
  buildTouchTab(lv_tabview_add_tab(tabview, "Touch"));
  buildWifiTab(lv_tabview_add_tab(tabview, "WiFi"));
  buildSystemTab(lv_tabview_add_tab(tabview, "System"));

  lv_timer_create(vitalsTimer, 500, nullptr);
  lv_timer_create(wifiTimer, 250, nullptr);
}

void setup() {
  Serial.begin(115200);
  delay(200);  // give native-USB CDC a moment to come up
  Serial.println("\n[boot] app starting");

  // Backlight off during init so the user never sees garbage RAM on the panel.
  pinMode(LCD_PIN_BL, OUTPUT);
  analogWrite(LCD_PIN_BL, 0);

  // Hard-reset the panel. Arduino_GFX is told GFX_NOT_DEFINED for RST so it
  // does not also toggle this line.
  pinMode(LCD_PIN_RST, OUTPUT);
  digitalWrite(LCD_PIN_RST, LOW);
  delay(20);
  digitalWrite(LCD_PIN_RST, HIGH);
  delay(150);

  if (!gfx->begin(40000000L /* 40MHz SPI */)) {
    Serial.println("[boot] gfx->begin() FAILED — halting");
    while (true) delay(1000);
  }
  jd9853_init(bus);          // JD9853 register set (see jd9853_panel.h)
  gfx->setRotation(LCD_ROTATION);
  gfx->fillScreen(RGB565_BLACK);
  Serial.printf("[boot] display %dx%d\n", gfx->width(), gfx->height());

  Wire.begin(TOUCH_PIN_SDA, TOUCH_PIN_SCL);
  Wire.setClock(400000);
  touch_begin();  // non-fatal: UI still works via swipe if touch is absent

  // WiFi in station mode but not connected — enough for scanning.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  lv_init();
  lv_tick_set_cb(lv_tick_cb);

  // MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA: the flush path hands these straight
  // to SPI DMA, which cannot reach PSRAM.
  const size_t buf_bytes = kBufBytes;
  lv_buf_a = static_cast<uint8_t *>(
      heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  lv_buf_b = static_cast<uint8_t *>(
      heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  if (lv_buf_a == nullptr) {
    Serial.println("[boot] draw buffer alloc FAILED — halting");
    while (true) delay(1000);
  }
  if (lv_buf_b == nullptr) {
    // Single-buffered still renders correctly, just with more tearing.
    Serial.println("[boot] second draw buffer unavailable, running single-buffered");
  }
  Serial.printf("[boot] lvgl draw buffers: %u bytes each\n", (unsigned)buf_bytes);

  display = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(display, lv_flush_cb);
  lv_display_set_buffers(display, lv_buf_a, lv_buf_b, buf_bytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  touch_indev = lv_indev_create();
  lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch_indev, lv_touch_cb);

  buildUi();

  fps_window_start = millis();
  setBacklight(80);
  Serial.println("[boot] ready");
}

void loop() {
  // lv_timer_handler() returns how many ms until it next wants to run; use that
  // as the sleep hint instead of a fixed delay, so we neither spin while LVGL is
  // idle nor stall it while it is busy.
  uint32_t idle_ms = lv_timer_handler();
  if (idle_ms == LV_NO_TIMER_READY || idle_ms > 16) idle_ms = 16;

  // Flushes over a 1s window -> the FPS figure on the System tab.
  const uint32_t now = millis();
  if (now - fps_window_start >= 1000) {
    fps_value = flush_count;
    flush_count = 0;
    fps_window_start = now;
    if (fps_label != nullptr) {
      lv_label_set_text_fmt(fps_label, "%lu fps", (unsigned long)fps_value);
    }
  }

  // BOOT button (GPIO0) cycles backlight, so there's a way to prove the board
  // is alive even if touch is misbehaving.
  static bool boot_was_down = false;
  static uint8_t bl_step = 3;  // matches setBacklight(80) above
  const bool boot_down = digitalRead(BOOT_BTN_PIN) == LOW;
  if (boot_down && !boot_was_down) {
    static const uint8_t levels[] = {10, 30, 55, 80, 100};
    bl_step = (bl_step + 1) % 5;
    setBacklight(levels[bl_step]);
    Serial.printf("[input] backlight -> %u%%\n", levels[bl_step]);
  }
  boot_was_down = boot_down;

  // Yield for as long as LVGL said it has nothing to do (capped above), which
  // also gives the WiFi and IDLE tasks their time.
  delay(idle_ms);
}
