#pragma once

#include "activities/RenderLock.h"

class GfxRenderer;

// --- TLS framebuffer borrow (constrained-heap fix, wolfSSL edition) ---
//
// Even with wolfSSL SP math (operand-sized bignums instead of mbedTLS's fixed
// 16KB record pair), a TLS session still wants more than the browsing
// activities can spare: the WOLFSSL object plus a per-record input buffer that
// grows to ~17KB when the server sends full-size 16KB records (BookFusion's
// large JSON responses do). The X3 browser enters these calls with ~40KB free
// and a ~25KB largest block — the handshake now succeeds, but a big response
// record can still fail to buffer.
//
// Fix: lend wolfSSL the one region guaranteed contiguous and idle during a
// network op — the e-ink framebuffer (~48-52KB). We register a multi_heap over
// it and install a global wolfSSL allocator hook (wolfSSL_SetAllocators) that
// routes only large (>=4KB) allocations there (session object, record
// buffers); small crypto temporaries stay on the normal heap. Nothing renders
// while a request runs — the panel keeps showing the pre-drawn status frame
// (e-ink holds it with no RAM).
//
// The hook is installed once and stays installed: when no borrow is active it
// degrades to plain malloc/free (the free callback routes by pointer range).
// The one fatal hazard is arena-allocated memory being freed AFTER the borrow
// ends — the destructor prevents it by closing every kept-alive wolfSSL session
// (BookFusion is the only one today) BEFORE deactivating the arena. If you add
// another module that holds a SecureHttpClient open between calls, close it too:
// its session object is sitting in the framebuffer, the next paint turns it
// into pixels, and the following request frees that wreckage inside
// wolfSSL_free().
//
// USAGE: scope this guard around a SINGLE network call (or a run of calls on
// one reused connection with NO rendering between them). Never nest it inside
// a held RenderLock — it takes one itself and the mutex is non-recursive, so
// nesting deadlocks. If a path renders between two requests (popups, epub
// reload), use a separate guard per request. The destructor closes the
// kept-alive connections BEFORE restoring rendering, so it must run before any
// post-network RenderLock draw. Long transfers (the EPUB download) may hold a
// borrow too, provided the UI is a pre-painted static frame — the e-ink holds
// it with no RAM — and any transient SecureHttpClient is destroyed (freeing
// its arena memory) before the borrow ends.
//
// PSRAM boards (X4 Pro): the arena is compiled out — active() is always false
// and the framebuffer is left untouched, because there the borrow only costs
// image integrity. Keeping the guard in the call sites is still correct: it
// holds the render lock and closes the connection. See DevicePolicy.h.
class TlsFramebufferBorrow {
 public:
  explicit TlsFramebufferBorrow(GfxRenderer& renderer);
  ~TlsFramebufferBorrow();

  TlsFramebufferBorrow(const TlsFramebufferBorrow&) = delete;
  TlsFramebufferBorrow& operator=(const TlsFramebufferBorrow&) = delete;

  // True if the framebuffer arena was installed (false => fell back to the
  // normal heap, e.g. framebuffer unavailable; the request still runs, just
  // without help).
  bool active() const { return installed_; }

 private:
  // Held for the whole borrow. Declared first so it is acquired before the
  // arena is installed and released after it is torn down.
  RenderLock lock_;
  bool installed_ = false;
};
