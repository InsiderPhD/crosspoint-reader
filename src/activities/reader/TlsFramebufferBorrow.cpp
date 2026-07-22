#include "TlsFramebufferBorrow.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <multi_heap.h>
#include <wolfssl/wolfcrypt/memory.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "BookFusionSyncClient.h"

namespace {

// One arena at a time (a sync never runs two TLS sessions concurrently). These
// back the global wolfSSL allocator hooks installed on first borrow.
multi_heap_handle_t g_fbHeap = nullptr;
uint8_t* g_fbStart = nullptr;
uint8_t* g_fbEnd = nullptr;
bool g_hooksInstalled = false;
// Route the session object and record buffers (>=4KB; the input buffer grows
// to ~17KB on full-size 16KB TLS records) into the arena; small crypto
// temporaries stay on the normal heap, which keeps its largest block intact.
constexpr size_t FB_MIN_ALLOC = 4096;

bool inArena(const void* p) {
  return p != nullptr && g_fbStart != nullptr && static_cast<const uint8_t*>(p) >= g_fbStart &&
         static_cast<const uint8_t*>(p) < g_fbEnd;
}

void* tlsMalloc(size_t size) {
  if (g_fbHeap != nullptr && size >= FB_MIN_ALLOC) {
    if (void* p = multi_heap_malloc(g_fbHeap, size)) {
      return p;
    }
    // Arena couldn't fit it — fall through to the normal heap rather than fail.
  }
  return malloc(size);
}

void tlsFree(void* p) {
  if (inArena(p)) {
    multi_heap_free(g_fbHeap, p);
    return;
  }
  free(p);
}

void* tlsRealloc(void* p, size_t size) {
  if (inArena(p)) {
    if (void* grown = multi_heap_realloc(g_fbHeap, p, size)) {
      return grown;
    }
    // Arena full — migrate the block to the normal heap.
    const size_t oldSize = multi_heap_get_allocated_size(g_fbHeap, p);
    void* moved = malloc(size);
    if (moved != nullptr) {
      memcpy(moved, p, oldSize < size ? oldSize : size);
      multi_heap_free(g_fbHeap, p);
    }
    return moved;
  }
  // Normal-heap pointer (or null): plain realloc. A null p with a large size
  // still prefers the arena via tlsMalloc's routing.
  if (p == nullptr) {
    return tlsMalloc(size);
  }
  return realloc(p, size);
}

}  // namespace

TlsFramebufferBorrow::TlsFramebufferBorrow(GfxRenderer& renderer) {
  // lock_ (render lock) is already held by the time we reach here (member init),
  // so the render task cannot draw into the framebuffer while we repurpose it.
  uint8_t* fb = renderer.getFrameBuffer();
  const size_t fbSize = renderer.getBufferSize();
  if (fb == nullptr || fbSize < FB_MIN_ALLOC * 2) {
    LOG_ERR("BFB", "framebuffer unavailable (%p / %u) - TLS borrow disabled", fb, (unsigned)fbSize);
    return;
  }

  // multi_heap / tlsf requires a 4-byte-aligned pool. Round the start up and
  // the size down to the alignment so registration always succeeds; we lose at
  // most a few bytes, still ample for the session object and record buffers.
  const uintptr_t raw = reinterpret_cast<uintptr_t>(fb);
  const uintptr_t aligned = (raw + 3u) & ~static_cast<uintptr_t>(3u);
  const size_t skip = static_cast<size_t>(aligned - raw);
  if (skip >= fbSize) {
    LOG_ERR("BFB", "framebuffer too small after alignment - TLS borrow disabled");
    return;
  }
  uint8_t* const alignedFb = reinterpret_cast<uint8_t*>(aligned);
  const size_t alignedSize = (fbSize - skip) & ~static_cast<size_t>(3u);

  g_fbHeap = multi_heap_register(alignedFb, alignedSize);
  if (g_fbHeap == nullptr) {
    LOG_ERR("BFB", "multi_heap_register failed (%u bytes @ %p) - TLS borrow disabled", (unsigned)alignedSize,
            alignedFb);
    return;
  }
  g_fbStart = alignedFb;
  g_fbEnd = alignedFb + alignedSize;

  // Install the hooks once; they stay installed for the firmware's lifetime and
  // degrade to plain malloc/free while no arena is active. (Unhooking would
  // race any wolfSSL memory still alive, and wolfSSL_SetAllocators rejects
  // NULL callbacks anyway.)
  if (!g_hooksInstalled) {
    if (wolfSSL_SetAllocators(tlsMalloc, tlsFree, tlsRealloc) != 0) {
      LOG_ERR("BFB", "wolfSSL_SetAllocators failed - TLS borrow disabled");
      // multi_heap has no unregister; clearing the range disables routing and
      // the pool memory is just the framebuffer, so nothing leaks.
      g_fbHeap = nullptr;
      g_fbStart = nullptr;
      g_fbEnd = nullptr;
      return;
    }
    g_hooksInstalled = true;
  }

  installed_ = true;
  LOG_DBG("BFB", "wolfSSL large allocs routed to %u-byte framebuffer arena", (unsigned)alignedSize);
}

TlsFramebufferBorrow::~TlsFramebufferBorrow() {
  // Close first so wolfSSL frees the session and record buffers via tlsFree
  // while the arena range is still valid. An arena pointer freed after the
  // range is cleared would hit free() and corrupt the heap.
  BookFusionSyncClient::closeConnection();
  if (installed_) {
    g_fbHeap = nullptr;
    g_fbStart = nullptr;
    g_fbEnd = nullptr;
    installed_ = false;
  }
  // lock_ is released after this; the framebuffer now holds TLS garbage, so
  // the caller's next paint must be a full redraw.
}
