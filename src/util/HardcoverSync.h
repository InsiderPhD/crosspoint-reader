#pragma once

#include <DevicePolicy.h>
#include <HardcoverSyncClient.h>

#include <cstdint>

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
#else
inline void preempt(uint32_t = 0) {}
inline void finishBeforeSleep(uint32_t = 0) {}
#endif

}  // namespace HardcoverSync
