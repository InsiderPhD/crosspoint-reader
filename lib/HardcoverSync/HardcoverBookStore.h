#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Everything learned about one local book on Hardcover, so that after the first
 * push each later one is a single request.
 *
 * Book and edition ids are global. The user_book and read ids belong to the
 * account; if they go stale (the book was removed on the website, or the token
 * now belongs to someone else) the push drops them and looks them up again.
 */
struct HardcoverBookLink {
  int32_t bookId = 0;     // 0 = not linked yet
  int32_t editionId = 0;  // 0 = unknown edition
  int32_t pages = 0;      // page count progress maps onto; 0 = unknown
  int32_t userBookId = 0;
  int32_t readId = 0;       // the open (unfinished) read being updated
  char startedAt[11] = {};  // that read's YYYY-MM-DD start date; empty = none
  int32_t syncedPages = -1;
  float syncedPercent = 0.0f;     // 0-100, last value Hardcover accepted
  uint32_t syncedThroughDay = 0;  // last local day ordinal whose progress was replayed
  bool finished = false;          // Hardcover already has this book as Read
  uint32_t lastMissEpoch = 0;     // last failed match; retried after a cool-off
};

/**
 * Per-book Hardcover sidecar.
 *
 * File path: /.crosspoint/hardcover_<md5_of_epub_path>.json
 */
class HardcoverBookStore {
 public:
  static bool load(const char* epubPath, HardcoverBookLink& out);
  static bool save(const char* epubPath, const HardcoverBookLink& link);

 private:
  static void buildSidecarPath(const char* epubPath, char* outPath, size_t maxLen);
};
