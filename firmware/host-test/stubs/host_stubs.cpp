// host_stubs.cpp — implementations for the Arduino/IDF surface the layer needs.
//
// Three groups, each small on purpose: Serial (recorded so tests can assert on
// what a script printed), the heap_caps_* shim over malloc, and the clock.

#include "Arduino.h"

#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "host_serial.h"

// ---------------------------------------------------------------- Serial

HostSerial Serial;

// Everything Serial has emitted this process. Kept whole rather than as a ring
// buffer: a test run prints little, and a truncated log is worse than a big one
// when something fails in CI.
static std::string g_serial_log;

// Echoing as we go rather than at exit means a crash (or an ASan abort) still
// leaves the output that led up to it visible in the CI log.
static void serial_emit(const char *s, size_t n) {
  g_serial_log.append(s, n);
  fwrite(s, 1, n, stdout);
  fflush(stdout);
}

void HostSerial::print(const char *s) {
  if (s) serial_emit(s, strlen(s));
}

void HostSerial::print(char c) { serial_emit(&c, 1); }

void HostSerial::println() { serial_emit("\n", 1); }

void HostSerial::println(const char *s) {
  print(s);
  println();
}

void HostSerial::write(uint8_t c) {
  const char ch = static_cast<char>(c);
  serial_emit(&ch, 1);
}

void HostSerial::printf(const char *fmt, ...) {
  // Two passes so a long line is never truncated: vsnprintf reports the length
  // it wanted, then we allocate exactly that. The layer prints stack traces
  // through here, which comfortably exceed any fixed buffer worth stack-space.
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  const int need = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  if (need < 0) {
    va_end(ap2);
    return;
  }
  std::vector<char> buf(static_cast<size_t>(need) + 1);
  vsnprintf(buf.data(), buf.size(), fmt, ap2);
  va_end(ap2);
  serial_emit(buf.data(), static_cast<size_t>(need));
}

size_t host_serial_mark() { return g_serial_log.size(); }

std::string host_serial_since(size_t mark) {
  if (mark >= g_serial_log.size()) return std::string();
  return g_serial_log.substr(mark);
}

bool host_serial_contains_since(size_t mark, const char *needle) {
  return host_serial_since(mark).find(needle) != std::string::npos;
}


// ---------------------------------------------------------------- chip identity

HostESP ESP;

uint32_t getCpuFrequencyMhz() { return 240; }

// ---------------------------------------------------------------- heap shim

// Outstanding bytes and blocks. Tracked with a header ahead of each allocation
// so free() knows the size; ASan still sees an ordinary malloc/free pair, which
// is what we want it instrumenting.
namespace {

struct BlockHeader {
  size_t size;
};

// The header is padded out to max_align_t so the pointer handed back keeps the
// alignment malloc promised. Without this the offset is sizeof(size_t) == 8 and
// every pointer comes back 8-mod-16, which is weaker than both malloc and the
// IDF allocator guarantee — so a host run could pass while the same code hit an
// alignment fault on a target that cares.
constexpr size_t kHeaderSize =
    ((sizeof(BlockHeader) + alignof(max_align_t) - 1) / alignof(max_align_t)) *
    alignof(max_align_t);

size_t g_live_bytes = 0;
size_t g_live_blocks = 0;

// A notional pool, only so the layer's "%u bytes free" diagnostics print
// something ordered and plausible. Not a claim about any real device.
constexpr size_t kNotionalPool = 8u * 1024u * 1024u;

void *alloc_tracked(size_t size) {
  // malloc(0) is allowed to return either NULL or a unique pointer; force the
  // latter so callers that only check for NULL as failure behave consistently.
  if (size == 0) size = 1;
  // Refuse rather than wrap: adding the header to a size near SIZE_MAX would
  // otherwise allocate something tiny and hand back a pointer the caller
  // believes is huge, which is a heap overflow rather than an allocation failure.
  if (size > SIZE_MAX - kHeaderSize) return nullptr;
  void *raw = malloc(kHeaderSize + size);
  if (!raw) return nullptr;
  static_cast<BlockHeader *>(raw)->size = size;
  g_live_bytes += size;
  g_live_blocks++;
  return static_cast<char *>(raw) + kHeaderSize;
}

}  // namespace

extern "C" {

void *heap_caps_malloc(size_t size, uint32_t) { return alloc_tracked(size); }

void *heap_caps_calloc(size_t n, size_t size, uint32_t) {
  // Overflow check before multiplying: the real allocator returns NULL rather
  // than wrapping, and a wrapped size here would be a heap overflow.
  if (n && size > SIZE_MAX / n) return nullptr;
  const size_t total = n * size;
  void *p = alloc_tracked(total);
  if (p) memset(p, 0, total);
  return p;
}

void heap_caps_free(void *ptr) {
  if (!ptr) return;
  void *raw = static_cast<char *>(ptr) - kHeaderSize;
  g_live_bytes -= static_cast<BlockHeader *>(raw)->size;
  g_live_blocks--;
  free(raw);
}

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps) {
  if (!ptr) return heap_caps_malloc(size, caps);
  if (size == 0) {
    heap_caps_free(ptr);
    return nullptr;
  }
  void *raw = static_cast<char *>(ptr) - kHeaderSize;
  const size_t old = static_cast<BlockHeader *>(raw)->size;
  void *fresh = heap_caps_malloc(size, caps);
  if (!fresh) return nullptr;  // original stays valid, as realloc promises
  memcpy(fresh, ptr, old < size ? old : size);
  heap_caps_free(ptr);
  return fresh;
}

uint32_t heap_caps_get_free_size(uint32_t) {
  return static_cast<uint32_t>(g_live_bytes >= kNotionalPool ? 0
                                                            : kNotionalPool - g_live_bytes);
}

uint32_t heap_caps_get_largest_free_block(uint32_t caps) {
  return heap_caps_get_free_size(caps);
}

uint32_t heap_caps_get_total_size(uint32_t) {
  return static_cast<uint32_t>(kNotionalPool);
}

size_t heap_caps_get_allocated_size(void *ptr) {
  if (!ptr) return 0;
  return static_cast<BlockHeader *>(
             static_cast<void *>(static_cast<char *>(ptr) - kHeaderSize))
      ->size;
}

size_t host_heap_live_bytes() { return g_live_bytes; }
size_t host_heap_live_blocks() { return g_live_blocks; }

}  // extern "C"

// ---------------------------------------------------------------- clock

// A virtual clock, advanced explicitly by the tests. Real time would make the
// timer tests either slow (sleeping for the interval) or flaky (racing it);
// stepping the clock makes "this timer fired twice" exact and instant.
static uint32_t g_millis = 0;

uint32_t millis() { return g_millis; }

void delay(uint32_t ms) { g_millis += ms; }

void host_clock_advance(uint32_t ms) { g_millis += ms; }
