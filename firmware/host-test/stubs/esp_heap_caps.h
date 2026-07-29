// esp_heap_caps.h — the host stand-in for the IDF capability allocator.
//
// Every heap_caps_* call maps onto plain malloc/free, which is the entire point
// of the host target: the JS heap and every binding allocation then run through
// the same allocator ASan instruments, so a use-after-free or a leak in the
// binding layer is caught here instead of on the next flash.
//
// The capability flags are accepted and ignored. On the board they choose
// between PSRAM and internal DMA-capable RAM, a distinction the host does not
// have and must not pretend to: nothing here can tell you whether an allocation
// would have fitted in the real pools. That is stated in docs/host-test.md so
// nobody reads a passing host run as evidence about memory on hardware.
//
// The one thing this shim does add is accounting. host_heap_live_bytes() reports
// the total currently outstanding, which is what lets the reload test assert
// that starting and stopping the same script N times returns to where it began.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_EXEC (1 << 0)
#define MALLOC_CAP_32BIT (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)

#ifdef __cplusplus
extern "C" {
#endif

void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);

// Synthetic, and only plausible enough to keep the layer's diagnostics honest:
// the free-size figures are a fixed notional pool minus what is outstanding.
// Never assert on these in a test — assert on host_heap_live_bytes() instead.
//
// These return uint32_t rather than size_t deliberately. On the ESP32 size_t is
// 32 bits, so the layer's `Serial.printf("%u bytes free", heap_caps_get_free_size(...))`
// is correct there; declaring them size_t here (64-bit) would make the same
// correct-on-target line produce a -Wformat warning on every host build, and the
// only ways to silence that are editing firmware to suit the host or turning the
// warning off. Matching the target's width is the honest fix.
uint32_t heap_caps_get_free_size(uint32_t caps);
uint32_t heap_caps_get_largest_free_block(uint32_t caps);
uint32_t heap_caps_get_total_size(uint32_t caps);

// QuickJS's usable_size hook must report 0 on the board (reporting real sizes
// corrupts the heap under IDF poisoning; docs/engine-notes.md). The host copy of
// that rule is enforced by jsvm_core.cpp itself, which never calls this.
size_t heap_caps_get_allocated_size(void *ptr);

// Bytes currently outstanding through the shim. The reload test's leak check.
size_t host_heap_live_bytes();

// Outstanding allocation count, which localizes a leak better than bytes alone.
size_t host_heap_live_blocks();

#ifdef __cplusplus
}
#endif
