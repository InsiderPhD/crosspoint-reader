#pragma once

#include <BookFusionSyncClient.h>

#include <cstdint>
#include <memory>
#include <string>

// Silent background reading-progress push (BookFusion or KOReader).
//
// Every existing sync path in the firmware is interactive: it owns the screen,
// blocks on the network, and often reboots afterwards to defragment the heap.
// Autosync is the opposite — it fires from a page turn, uploads on a background
// task, and the user never sees it. That means it is the one place where WiFi
// runs *concurrently* with EPUB rendering, so the gates below (heap, cooldown,
// single-flight) are load-bearing rather than defensive:
//
//   * The WiFi stack holds ~40-45KB and a TLS handshake peaks ~35KB on top.
//   * Grayscale rendering wants six contiguous 8KB chunks, and a chapter
//     re-parse is heap-hungry — both can OOM if we start a sync at a bad moment.
//
// Push-only by design. Applying a remote position mid-book would move the page
// under the reader's thumb; the manual menu sync stays the merge/pull path.
//
// Gated on TimeUtils::wasTimeSyncedThisBoot(): the boot NTP attempt is the
// firmware's proof that WiFi works this session, and a valid clock is required
// for the sync timestamps to be meaningful.
namespace ProgressAutoSync {

enum class Provider : uint8_t { None, BookFusion, KOReader };

// Result of the cheap main-task gate check.
struct Trigger {
  Provider provider = Provider::None;
  uint32_t bookId = 0;  // valid when provider == BookFusion
};

// Everything the push needs, snapshotted on the main task while `epub` and
// `section` are alive. The background task owns this and frees it, so it
// safely outlives the EpubReaderActivity that built it — the task must never
// dereference the activity, the Epub, or the Section.
struct Payload {
  Provider provider = Provider::None;
  std::string epubPath;

  // BookFusion
  uint32_t bookId = 0;
  BookFusionPosition bfPos;
  uint64_t totalReadingMs = 0;  // READING_STATS snapshot; 0 = skip time tracking

  // KOReader. koDocumentHash empty means "compute the binary hash in the task"
  // (it reads the EPUB off SD, which is too slow for a page turn).
  std::string koDocumentHash;
  std::string koXpath;
  float koPercentage = 0.0f;  // 0.0-1.0

  // Shared bookkeeping for the baseline sidecars.
  int spineIndex = 0;
  int pageNumber = 0;
  int totalPages = 0;
  float bookPercent = 0.0f;  // 0-100
};

// Main-task, non-blocking. Applies every gate: setting mode, NTP-synced boot,
// provider availability, threshold crossing, cooldown, single-flight, heap.
// Returns Provider::None when any gate fails. Safe to call on every page turn.
Trigger shouldSync(const std::string& epubPath, int spineIndex, float bookPercent);

// Provider gates only — no threshold, cooldown or heap check. For the On Exit
// mode, which must push whatever is outstanding when the book closes.
Trigger providerFor(const std::string& epubPath);

// Spawn the detached push task, transferring ownership of the payload.
// Returns false if the task could not be created (payload is freed either way).
bool start(std::unique_ptr<Payload> payload);

bool isBusy();

// True exactly once after a background push finished. The reader uses this to
// schedule a full refresh, since any page rendered while WiFi held its heap
// may have fallen back from the grayscale path.
bool consumeCompletion();

// Cooperatively stop an in-flight push and wait (bounded) until the radio is
// released. Call before anything that needs the WiFi heap or the radio itself
// (library recache, manual sync, WiFi selection). No-op when idle.
void preempt(uint32_t maxWaitMs = 12000);

// Blocking connect -> push -> teardown on the calling task. Used by the On Exit
// mode: a detached task would be killed mid-TLS by the deep sleep that follows
// the reader's onExit(). Never draws UI.
bool pushBlocking(const Payload& payload);

// True when the payload position is ahead of the persisted last-synced
// baseline. Forward-only: a position behind the baseline reports false so a
// re-read never clobbers the server.
bool hasUnsyncedProgress(const Payload& payload);

// Drop the cached per-book baseline and provider. Call when a book is opened.
void resetSessionBaseline();

}  // namespace ProgressAutoSync
