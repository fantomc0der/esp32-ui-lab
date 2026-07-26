// board_config.h — selects the target board and declares what it can do.
//
// This is the ONLY file that knows more than one board exists. Everything else
// includes this and reads the BOARD_* flags; nothing else contains an #ifdef on
// a board name. Adding a third board means adding a block here plus its pin
// header and a touch driver, and touching nothing else.
//
// Pick a target at compile time by defining exactly one of:
//   BOARD_WAVESHARE_S3_147   Waveshare ESP32-S3-Touch-LCD-1.47  (default)
//   BOARD_CYD_2432S028R      ESP32-2432S028R "Cheap Yellow Display"
//
// flash.ps1 -Target passes the right -D. Building with none of them defined
// selects the Waveshare board, so existing builds behave exactly as before.
#pragma once

#include <esp_heap_caps.h>

#if !defined(BOARD_WAVESHARE_S3_147) && !defined(BOARD_CYD_2432S028R)
#define BOARD_WAVESHARE_S3_147
#endif

#if defined(BOARD_WAVESHARE_S3_147) && defined(BOARD_CYD_2432S028R)
#error "Define only one BOARD_* target."
#endif

// ---------------------------------------------------------------------------
#if defined(BOARD_WAVESHARE_S3_147)

#include "board_waveshare_s3_147_pins.h"

#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-1.47"

// JS heap lives in the 8 MB of octal PSRAM.
#define BOARD_JS_HEAP_CAPS MALLOC_CAP_SPIRAM
// Large buffers (script source, fetch bodies) also go to PSRAM.
#define BOARD_BULK_CAPS    MALLOC_CAP_SPIRAM

#define BOARD_HAS_PSRAM      1
#define BOARD_HAS_FATFS      1  // 9.9 MB FATFS partition can hold app.js
#define BOARD_HAS_SDMMC      1  // 4-bit SDMMC
#define BOARD_HAS_SD_SPI     0
#define BOARD_HAS_BATTERY    1  // GPIO12 via a 1/2 divider
#define BOARD_HAS_WIFI       1
#define BOARD_TOUCH_AXS5106L 1
#define BOARD_TOUCH_XPT2046  0

// Draw buffers: two at 1/8 screen each. Plenty of room for that here, and
// bigger buffers mean fewer flush calls per frame.
#define BOARD_DRAW_BUF_DIVISOR 8
#define BOARD_DOUBLE_BUFFER    1

// Stock LVGL widget pool; PSRAM means nothing competes for internal RAM.
#ifndef BOARD_LV_MEM_KB
#define BOARD_LV_MEM_KB 48
#endif

// Loop-task C stack, and the JS recursion budget carved out of it.
#ifndef BOARD_LOOP_STACK_KB
#define BOARD_LOOP_STACK_KB 32
#endif

// ---------------------------------------------------------------------------
#elif defined(BOARD_CYD_2432S028R)

#include "board_cyd_2432s028r_pins.h"

#define BOARD_NAME "ESP32-2432S028R (CYD)"

// No PSRAM at all, so the JS heap comes from internal RAM — and the DMA cap is
// REQUIRED, not decorative. Plain MALLOC_CAP_INTERNAL on a classic ESP32 can
// return IRAM (0x400xxxxx), which permits only aligned 32-bit access, while
// QuickJS writes bytes and shorts throughout its heap. That combination panics
// with LoadStoreError/EXCCAUSE 0x03 inside JS_NewRuntime2 — observed on this
// board at EXCVADDR=0x40091d18. MALLOC_CAP_DMA additionally guarantees
// byte-addressable DRAM (0x3FFxxxxx), which is what QuickJS needs. The S3 never
// hits this because its PSRAM is always DRAM.
//
// Note also that heap_caps_get_free_size(MALLOC_CAP_INTERNAL) OVERSTATES what
// the VM can use here by ~58 kB, because it counts that unusable IRAM.
#define BOARD_JS_HEAP_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)
#define BOARD_BULK_CAPS    (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)

#define BOARD_HAS_PSRAM      0
#define BOARD_HAS_FATFS      0  // 4 MB flash leaves no room for a script partition
#define BOARD_HAS_SDMMC      0
#define BOARD_HAS_SD_SPI     1  // microSD over SPI instead
#define BOARD_HAS_BATTERY    0  // no battery sense circuit
#define BOARD_TOUCH_AXS5106L 0
#define BOARD_TOUCH_XPT2046  1

// WiFi stays enabled, which is the whole reason for the two cuts below.
//
// Measured on hardware, internal DRAM at each boot stage: 158776 free at start,
// then WiFi.mode() takes 51740, LVGL 39276, the SD driver 30204 — leaving 37556
// against the ~80736 QuickJS needs, so the VM could not start. The radio is the
// single largest consumer, but a board that cannot make an HTTP request is a
// different (and much less useful) product, so instead of dropping it we buy the
// memory back from the display stack:
//
//   draw buffers 1/8 -> 1/16 screen   frees 19200
//   LV_MEM_SIZE  48 KB -> 24 KB       frees 24576
//                                     ------------
//                                     43776, vs the 43180 shortfall
//
// The cost is more flush calls per frame (a mostly-static UI barely notices) and
// a smaller LVGL widget pool, which is still ample for these apps.
#define BOARD_HAS_WIFI       1

// One buffer at 1/16 screen, rather than the usual two at 1/8.
//
// Single-buffering costs some flush parallelism (LVGL cannot render the next
// region while the current one is going out over SPI), which a mostly-static UI
// barely notices, and hands 19200 bytes straight back to the JS heap.
#define BOARD_DRAW_BUF_DIVISOR 16
#define BOARD_DOUBLE_BUFFER    0

// Caps LVGL's internal widget pool. flash.ps1 passes this as -D so it also
// reaches LVGL's own sources via lv_conf.h; the fallback here keeps a manual
// arduino-cli build (without the -D) consistent with a scripted one.
#ifndef BOARD_LV_MEM_KB
#define BOARD_LV_MEM_KB 24
#endif

// Loop-task C stack: the same 32 KB as the other target, because this stack is
// allocated from the same internal DRAM as the JS heap. Growing it to 48 KB to
// make room for the JS budget cost 16 KB of heap and halved the largest free
// block, which broke context creation. The JS budget (JS_MAX_STACK, 12 KB here)
// is shrunk instead — see flash.ps1.
#ifndef BOARD_LOOP_STACK_KB
#define BOARD_LOOP_STACK_KB 32
#endif

#endif
