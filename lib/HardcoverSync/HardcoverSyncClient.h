#pragma once

#include <cstddef>
#include <cstdint>

/** A day's reading, replayed to Hardcover as a progress update dated that day. */
struct HardcoverDayProgress {
  uint32_t dayOrdinal = 0;  // local day number (TimeUtils day ordinal)
  char date[11] = {};       // the same day as YYYY-MM-DD
  uint8_t percent = 0;      // book progress at the end of that day's last session
};

/**
 * One local book, as the Hardcover push sees it. Plain pointers: the caller
 * keeps the strings and the days array alive for the duration of the call.
 */
struct HardcoverBookInfo {
  const char* epubPath = nullptr;  // sidecar key
  const char* isbnPath = nullptr;  // Epub::getIsbnPath(); the file may not exist
  const char* title = nullptr;
  const char* author = nullptr;
  // Per-day history, ascending by dayOrdinal. Days Hardcover hasn't had yet are
  // replayed (oldest first, a bounded number per push) before the current
  // position. todayOrdinal 0 = clock unknown: every listed day is a replay.
  const HardcoverDayProgress* days = nullptr;
  size_t dayCount = 0;
  uint32_t todayOrdinal = 0;
};

/** What a successful push left on Hardcover, for the result screen. */
struct HardcoverPushResult {
  int32_t pages = 0;       // page reported; 0 when the edition has no page count
  int32_t totalPages = 0;  // 0 when unknown
  bool markedRead = false;
};

/**
 * Client for the Hardcover.app GraphQL API (https://api.hardcover.app/v1/graphql).
 *
 * Authentication is the user's personal token as a Bearer header. The query and
 * mutation shapes follow the KOReader plugin (Billiam/hardcoverapp.koplugin),
 * which is the long-running reference for driving this API from an e-reader.
 *
 * A push is:
 *   1. Link the book once: edition by ISBN-13/10, else a title search whose
 *      result must match the EPUB title. Misses are cached for a week so an
 *      unknown book doesn't cost a search on every sync.
 *   2. Find the user's copy (user_book) and its open read; add the book as
 *      Currently Reading and start a read when either is missing.
 *   3. Replay each day Hardcover hasn't had (dated progress updates), then
 *      write the current progress_pages; on finish, set the book to Read.
 * After the first push only step 3 hits the network.
 *
 * Every call opens and closes its own connection, so nothing wolfSSL allocated
 * outlives it — required inside a TlsFramebufferBorrow, and it keeps the
 * background worker free of state shared with the main task.
 */
class HardcoverSyncClient {
 public:
  enum Error {
    OK = 0,
    NO_TOKEN,
    NETWORK_ERROR,
    AUTH_FAILED,   // token rejected or expired
    RATE_LIMITED,  // 429; the API allows 60 requests per minute
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,  // no Hardcover book matched this EPUB
  };

  // Push progress for one book. percent is 0-100; finished marks the book Read.
  // todayIso ("YYYY-MM-DD") dates a newly started read; null leaves it unset.
  static Error pushProgress(const HardcoverBookInfo& book, float percent, bool finished, const char* todayIso,
                            HardcoverPushResult* out = nullptr);

  // True when there is nothing to push (per the sidecar): Hardcover already
  // holds this position, or the book recently failed to match. Lets a caller
  // skip bringing WiFi up at all. SD read only, no network.
  static bool isUpToDate(const char* epubPath, float percent, bool finished);

  // Forget what this book last sent (position, finished, replayed days, the
  // user's copy and read ids, a cached miss) so the next push re-sends it all
  // and looks the book up on Hardcover again. For when the data was deleted on
  // the website. The book/edition link survives — it doesn't depend on the
  // account's data. SD only, no network.
  static void forgetSyncState(const char* epubPath);

  static const char* errorString(Error error);
};
