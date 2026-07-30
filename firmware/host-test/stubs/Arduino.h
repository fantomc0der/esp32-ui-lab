// Arduino.h — the host stand-in for the Arduino core.
//
// The binding layer includes <Arduino.h> for two things only: Serial, and
// millis(). This provides exactly those, plus the handful of integer typedefs
// that come with the real header. It is deliberately not a general Arduino
// emulation: every symbol here exists because a file under test names it, and
// anything the layer grows later should fail to compile loudly rather than
// silently bind to an approximation.
//
// Serial is the interesting one. Scripts report through console.log, and the
// binding layer reports errors the same way, so Serial *is* the observable
// output of a running script. It therefore both echoes to stdout (so a failing
// CI run shows what happened) and records into a buffer the tests can assert
// on. See host_serial.h for that half.
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The ESP32 core's Arduino.h reaches esp_heap_caps.h transitively, and
// bindings_fs.cpp relies on that: it calls heap_caps_malloc() without including
// the header itself. Mirroring the real include graph keeps the stub honest — a
// file that compiles on the board compiles here for the same reason.
#include "esp_heap_caps.h"

// The real core defines these; LVGL and the binding layer both assume them.
typedef uint8_t byte;
typedef bool boolean;

// Milliseconds since start. Monotonic, and also what drives lv_tick.
uint32_t millis();
void delay(uint32_t ms);

// A cursor into Serial's recorded output, so a test can scope an assertion to
// "what this script printed" rather than everything since process start.
size_t host_serial_mark();

// The chip-identity surface sys.info() reports. Fixed values describing the
// board this repo targets, so a script reading sys.info() on the host sees the
// same shape it would on the panel. They are labels, not measurements: nothing
// here can tell you anything about real silicon.
class HostESP {
 public:
  const char *getChipModel() const { return "ESP32-S3"; }
  uint8_t getChipRevision() const { return 0; }
  uint8_t getChipCores() const { return 2; }
  uint32_t getFlashChipSize() const { return 16u * 1024u * 1024u; }
  size_t getPsramSize() const { return 8u * 1024u * 1024u; }
};

extern HostESP ESP;

uint32_t getCpuFrequencyMhz();

// Serial, reduced to the four calls the layer makes: print, println, printf,
// and write. Output goes to stdout and to the recording buffer.
class HostSerial {
 public:
  void print(const char *s);
  void print(char c);
  void println();
  void println(const char *s);
  void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void write(uint8_t c);

  // Present so host code can drive the same shape as a sketch; no-ops here.
  void begin(unsigned long) {}
  void flush() {}
  operator bool() const { return true; }
};

extern HostSerial Serial;
