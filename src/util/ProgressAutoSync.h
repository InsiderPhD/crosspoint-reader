#pragma once

#include <BookFusionSyncClient.h>

#include <cstdint>
#include <memory>
#include <string>

// Silent reading-progress push (BookFusion or KOReader). No popups, no screen
// ownership — the only sync path the user never sees.
//
// Push-only by design. Applying a remote position mid-book would move the page
// under the reader's thumb; the manual menu sync stays the merge/pull path.
//
// WHY THIS BLOCKS RATHER THAN RUNNING IN THE BACKGROUND
// An earlier version pushed from a FreeRTOS task so reading stayed responsive.
// It could never fire. Measured on device: the reader idles at 67-78KB free
// with the chapter layout and book metadata loaded, the WiFi stack takes ~40KB,
// and the TLS handshake needs two 16KB mbedTLS record buffers (fixed in the
// precompiled library) plus X509/RSA working space on top. That is a ~20-35KB
// shortfall that no amount of waiting for a good moment recovers. The manual
// sync only fits because it releases the section and the Epub first — its own
// notes record MinFree bottoming out at 184 bytes with just the section freed.
// So autosync does the same: free, push, reload. The cost is a few seconds of
// unresponsive buttons; the alternative was a feature that never ran.
//
// ARM NOW, FIRE ON PAGE 2
// A threshold crossing only arms the sync. It runs when the reader reaches the
// second page of a chapter, which is the furthest point from
// silentIndexNextChapterIfNeeded() — the penultimate-page build of the next
// chapter, and the allocation most likely to die if it collides with WiFi.
//
// Gated on TimeUtils::wasTimeSyncedThisBoot(): the boot NTP attempt is the
// firmware's proof that WiFi works this session, and a valid clock is required
// for the sync timestamps to be meaningful.
namespace ProgressAutoSync {

enum class Provider : uint8_t { None, BookFusion, KOReader };

struct Trigger {
  Provider provider = Provider::None;
  uint32_t bookId = 0;  // valid when provider == BookFusion
};

// Everything a push needs, snapshotted while `epub` and `section` are alive —
// the sync releases both before opening TLS, so nothing here may point at them.
struct Payload {
  Provider provider = Provider::None;
  std::string epubPath;

  // BookFusion
  uint32_t bookId = 0;
  BookFusionPosition bfPos;
  uint64_t totalReadingMs = 0;  // READING_STATS snapshot; 0 = skip time tracking

  // KOReader
  std::string koDocumentHash;  // empty => resolved during the push
  std::string koXpath;
  float koPercentage = 0.0f;  // 0.0-1.0

  // Shared bookkeeping for the baseline sidecars.
  int spineIndex = 0;
  int pageNumber = 0;
  int totalPages = 0;
  float bookPercent = 0.0f;  // 0-100
};

// Cheap main-task check for a threshold crossing. Applies setting mode, NTP,
// provider availability, cooldown and a forward-only threshold. On a crossing
// it arms the sync and returns true; the push itself waits for page 2.
bool armIfThresholdCrossed(const std::string& epubPath, int spineIndex, float bookPercent);

// True when a crossing is waiting to be pushed.
bool isArmed();
void disarm();

// Provider gates only — no threshold or cooldown. Used by On Exit mode.
Trigger providerFor(const std::string& epubPath);

// Connect to the last-known network, push, tear WiFi down. Blocking, never
// draws UI. The caller MUST have released the section and Epub first — see the
// header comment; without that the TLS handshake will not fit.
bool pushBlocking(const Payload& payload);

// Free heap after the caller's release step, below which the push is skipped
// rather than risking an OOM. Diagnostics for the same are logged either way.
bool heapAllowsPush();

// True when the payload position is ahead of the persisted last-synced
// baseline. Forward-only: a position behind the baseline reports false so a
// re-read never clobbers the server.
bool hasUnsyncedProgress(const Payload& payload);

// Drop the cached per-book baseline, provider and armed state. Call on open.
void resetSessionBaseline();

}  // namespace ProgressAutoSync
