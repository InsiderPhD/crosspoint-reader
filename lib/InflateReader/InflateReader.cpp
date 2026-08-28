#include "InflateReader.h"

#include <DevicePolicy.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <type_traits>

namespace {
constexpr size_t INFLATE_DICT_SIZE = INFLATE_DICT_BYTES;

// Single global slot: a caller-lent dictionary buffer, and whether a reader has
// taken it. Not a stack of leases — one section build is in flight at a time, and
// a second concurrent reader must fall back to malloc rather than share a window.
uint8_t* g_scratchBuffer = nullptr;
size_t g_scratchLength = 0;
bool g_scratchInUse = false;
// NOTE: a static BSS pool for this dictionary was tried and reverted. It made
// section builds immune to heap fragmentation, but the permanent 32KB pushed
// "reader + BLE stack" past the total heap budget (observed: 4KB free after
// enabling Bluetooth with a book open → OOM abort). If builds under
// fragmentation need a guaranteed dictionary again, borrow an idle 48KB
// framebuffer-sized region for the duration of the build instead of pinning
// RAM permanently.
}  // namespace

// Guarantee the cast pattern in the header comment is valid.
static_assert(std::is_standard_layout<InflateReader>::value,
              "InflateReader must be standard-layout for the uzlib callback cast to work");

uint8_t* borrowInflateScratch(const size_t needed) {
  if (!g_scratchBuffer || g_scratchInUse || needed == 0 || needed > g_scratchLength) return nullptr;
  g_scratchInUse = true;
  return g_scratchBuffer;
}

bool returnInflateScratch(const uint8_t* buffer) {
  if (!buffer || buffer != g_scratchBuffer) return false;
  g_scratchInUse = false;
  return true;
}

InflateReader::~InflateReader() { deinit(); }

bool InflateReader::init(const bool streaming) {
  deinit();  // free any previously allocated ring buffer and reset state

  if (streaming) {
    // Prefer a leased buffer (see InflateScratchLease): it is contiguous by
    // construction, whereas the heap may have no 32KB block left at all.
    if (g_scratchBuffer && !g_scratchInUse && g_scratchLength >= INFLATE_DICT_SIZE) {
      g_scratchInUse = true;
      ringBufferBorrowed = true;
      ringBuffer = g_scratchBuffer;
      memset(ringBuffer, 0, INFLATE_DICT_SIZE);
      uzlib_uncompress_init(&decomp, ringBuffer, INFLATE_DICT_SIZE);
      LOG_DBG("INF", "Using leased %u-byte dictionary (no heap allocation)", (unsigned)INFLATE_DICT_SIZE);
      return true;
    }
    ringBuffer = static_cast<uint8_t*>(malloc(INFLATE_DICT_SIZE));
    if (!ringBuffer) {
      // This single 32KB request is the allocation that fails first on this
      // device, and free-byte totals never explain why. Dump the region map at
      // the exact moment of failure: it shows whether the heap is genuinely
      // exhausted, or merely cut into pieces none of which is big enough — and
      // in the latter case, which region and how many free blocks it holds.
      LOG_ERR("INF", "Inflate dictionary malloc(%u) FAILED: free=%u largest=%u", (unsigned)INFLATE_DICT_SIZE,
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      heap_caps_print_heap_info(MALLOC_CAP_8BIT);
      return false;
    }
    memset(ringBuffer, 0, INFLATE_DICT_SIZE);
  }

  uzlib_uncompress_init(&decomp, ringBuffer, ringBuffer ? INFLATE_DICT_SIZE : 0);
  return true;
}

void InflateReader::deinit() {
  if (ringBuffer) {
    if (ringBufferBorrowed) {
      g_scratchInUse = false;
      ringBufferBorrowed = false;
    } else {
      free(ringBuffer);
    }
    ringBuffer = nullptr;
  }
  memset(&decomp, 0, sizeof(decomp));
}

void InflateReader::setSource(const uint8_t* src, size_t len) {
  decomp.source = src;
  decomp.source_limit = src + len;
}

void InflateReader::setReadCallback(int (*cb)(struct uzlib_uncomp*)) { decomp.source_read_cb = cb; }

void InflateReader::skipZlibHeader() {
  uzlib_get_byte(&decomp);
  uzlib_get_byte(&decomp);
}

bool InflateReader::read(uint8_t* dest, size_t len) {
  if (!ringBuffer) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when read() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0) return false;
  return decomp.dest == decomp.dest_limit;
}

InflateStatus InflateReader::readAtMost(uint8_t* dest, size_t maxLen, size_t* produced) {
  if (!ringBuffer) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when readAtMost() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + maxLen;

  const int res = uzlib_uncompress(&decomp);
  *produced = static_cast<size_t>(decomp.dest - dest);

  if (res == TINF_DONE) return InflateStatus::Done;
  if (res < 0) return InflateStatus::Error;
  return InflateStatus::Ok;
}

InflateScratchLease::InflateScratchLease(uint8_t* buffer, const size_t length) {
#if !CROSSPOINT_FB_SCRATCH_BORROW
  // PSRAM-equipped board: a 32KB contiguous malloc is never in doubt, so don't
  // clobber the framebuffer for it. See DevicePolicy.h.
  (void)buffer;
  (void)length;
  return;
#else
  if (!buffer || length < INFLATE_DICT_SIZE || g_scratchBuffer != nullptr) {
    // Too small, or a lease is already outstanding — readers keep using malloc.
    return;
  }
  g_scratchBuffer = buffer;
  g_scratchLength = length;
  g_scratchInUse = false;
  registered = true;
#endif
}

InflateScratchLease::~InflateScratchLease() {
  if (!registered) return;
  g_scratchBuffer = nullptr;
  g_scratchLength = 0;
  g_scratchInUse = false;
}
