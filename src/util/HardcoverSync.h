#pragma once

#include <DevicePolicy.h>
#include <HardcoverSyncClient.h>

#include <cstdint>

class GfxRenderer;
struct ReadingBookStats;

// Hardcover sync, driven from the reading-stats store: a book's progress is its
// stats record (lastProgressPercent, completed), so a push needs no open Epub.
//
// Two ways in:
//   * Settings > Stats > Push to Hardcover, on every device (HardcoverPushActivity).
//   * CROSSPOINT_HARDCOVER_AUTO_SYNC boards only: onSessionWritten(), called by
//     ReadingStatsStore::endSession, queues a silent push on a background task.
namespace HardcoverSync {

// A stats record worth pushing: a real book path with some progress.
bool isEligible(const ReadingBookStats& book);

// True when Hardcover already has this book's current progress. SD only.
bool isUpToDate(const ReadingBookStats& book);

// Blocking push of one book. WiFi must already be up. On a tight-heap board the
// caller wraps this in a TlsFramebufferBorrow.
HardcoverSyncClient::Error pushBook(const ReadingBookStats& book, HardcoverPushResult* out = nullptr);

// Make the next push of `book` re-send everything (see
// HardcoverSyncClient::forgetSyncState).
void forgetSyncState(const ReadingBookStats& book);

#if CROSSPOINT_HARDCOVER_AUTO_SYNC
// Queue a background push of `book` if it has unsynced progress. Main task only.
// Only the latest request per task run is kept; older queued ones are replaced.
void onSessionWritten(const ReadingBookStats& book);

// Stop the background push and wait (bounded) until it has released WiFi. For
// foreground code about to use the radio itself.
void preempt(uint32_t maxWaitMs = 8000);

// Let an in-flight or queued push finish (bounded) before deep sleep cuts power.
// Sessions usually end by sleeping from the reader, so cancelling here would drop
// most automatic pushes.
void finishBeforeSleep(uint32_t maxWaitMs = 30000);

// True when a whole-library push is due: there is a token, the wall clock is
// trustworthy, and the last full sync ran at least a calendar day ago (or never).
// Cheap - reads only the token store and APP_STATE, touches no network.
bool isFullSyncDue();

// Push every eligible, out-of-date book on the CALLING task, reusing a WiFi
// connection the caller already has up (it neither connects nor tears down).
// Records the run against today's date whether or not every book succeeded, so
// a persistently failing API cannot retry on every boot.
//
// The caller's task needs a TLS-sized stack - see kWorkerStackBytes; the 4KB a
// plain helper task gets is not enough for a wolfSSL handshake.
//
// Bounded twice over, because this runs on the boot timeline: it stops at
// budgetMs of wall clock, and after kMaxConsecutiveFailures failed books, so a
// dead endpoint costs a couple of request timeouts rather than one per book.
// stopFlag, when non-null, is polled between books for cooperative preemption.
// Returns the number of books successfully pushed.
int runFullSync(uint32_t budgetMs, const volatile bool* stopFlag = nullptr);

// Tight-heap boards have no background push; see pushBeforeSleep.
inline void pushBeforeSleep(GfxRenderer&) {}
#else
inline void preempt(uint32_t = 0) {}
inline void finishBeforeSleep(uint32_t = 0) {}
inline bool isFullSyncDue() { return false; }
inline int runFullSync(uint32_t, const volatile bool* = nullptr) { return 0; }

// Tight-heap boards (X3/X4) never push from inside the reader. Instead, once
// the sleep screen is on the panel and the reader's heap is released, push up
// to three out-of-date books (the one just closed first) on the calling task:
// silent WiFi to the last network, TLS in the borrowed framebuffer, WiFi off.
// Blocks sleep entry for a few seconds, and only when something is unsynced.
// Call after anything that still needs the framebuffer contents.
void pushBeforeSleep(GfxRenderer& renderer);
#endif

}  // namespace HardcoverSync
