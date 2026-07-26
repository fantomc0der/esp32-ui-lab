// board_storage.h — where scripts come from, per board.
//
// The two boards differ in both the bus and the number of filesystems:
//
//   Waveshare  4-bit SDMMC card, PLUS a 9.9 MB FATFS partition in its 16 MB
//              flash. Two places a script can live, hence the "flash:" path
//              prefix that picks the partition explicitly.
//   CYD        microSD over SPI only. Its 4 MB flash has no room for a script
//              partition once a 3 MB app is in it, so there is exactly one
//              filesystem and "flash:" resolves nowhere.
//
// This header hides that behind board_storage_begin() plus two accessors that
// return nullptr when a filesystem does not exist on this board. Callers check
// for nullptr, which they had to do anyway for an unmounted card.
#pragma once

#include <Arduino.h>
#include <FS.h>

#include "board_config.h"

#if BOARD_HAS_SDMMC
#include <SD_MMC.h>
#endif
#if BOARD_HAS_SD_SPI
#include <SD.h>
#include <SPI.h>
#endif
#if BOARD_HAS_FATFS
#include <FFat.h>
#endif

namespace board_storage {

inline bool g_sd_ok = false;
inline bool g_flash_ok = false;

#if BOARD_HAS_SD_SPI
// The CYD's card shares a bus with the touch controller, so it gets its own
// SPIClass instance rather than the global SPI object the display may be using.
inline SPIClass g_sd_spi(VSPI);
#endif

// Mounts whatever this board has. Mounted once and kept: that is what gives
// scripts a real filesystem, at the cost of needing a reset to swap cards.
inline void begin() {
#if BOARD_HAS_SDMMC
  SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0, SD_PIN_D1, SD_PIN_D2, SD_PIN_D3);
  g_sd_ok = SD_MMC.begin("/sdcard", false /* 4-bit */) && SD_MMC.cardType() != CARD_NONE;
#elif BOARD_HAS_SD_SPI
  g_sd_spi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
  g_sd_ok = SD.begin(SD_PIN_CS, g_sd_spi) && SD.cardType() != CARD_NONE;
#endif

#if BOARD_HAS_FATFS
  g_flash_ok = FFat.begin(true /* format on first use */);
#else
  g_flash_ok = false;
#endif
}

// The card, or nullptr when absent or this board has no card slot.
inline fs::FS *sd() {
  if (!g_sd_ok) return nullptr;
#if BOARD_HAS_SDMMC
  return static_cast<fs::FS *>(&SD_MMC);
#elif BOARD_HAS_SD_SPI
  return static_cast<fs::FS *>(&SD);
#else
  return nullptr;
#endif
}

// The flash script partition, or nullptr on boards without one.
inline fs::FS *flash() {
#if BOARD_HAS_FATFS
  return g_flash_ok ? static_cast<fs::FS *>(&FFat) : nullptr;
#else
  return nullptr;
#endif
}

}  // namespace board_storage
