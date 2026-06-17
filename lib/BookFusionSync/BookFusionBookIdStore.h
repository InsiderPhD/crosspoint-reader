#pragma once
#include <cstddef>
#include <cstdint>

struct BookFusionStoredPosition {
  float percentage = 0.0f;
  float pagePositionInBook = 0.0f;
  int chapterIndex = 0;
  // Exact page coordinates from the moment we synced. Used for an integer-
  // exact "did local advance?" check that doesn't fall through to the
  // floating-point epsilon, which can hide a 2-page advance in books with
  // many chapters. -1/0 mean the sidecar predates this field or was written
  // by an apply-remote (where local pageNumber isn't known yet).
  int pageNumber = -1;
  int totalPages = 0;
};

/**
 * Per-book sidecar for BookFusion book IDs.
 *
 * Each EPUB that was downloaded from BookFusion has an associated sidecar file
 * at /.crosspoint/bookfusion_<md5_of_epub_path>.json containing its numeric book_id.
 *
 * Returns 0 from loadBookId() when no sidecar exists — 0 is never a valid BookFusion ID.
 */
class BookFusionBookIdStore {
 public:
  // Load book_id for the given epub path. Returns 0 if not a BookFusion book.
  static uint32_t loadBookId(const char* epubPath);

  // Cheap "is this a BookFusion book?" check — sidecar existence only, no parse.
  static bool hasBookId(const char* epubPath);

  // Save book_id for an epub path. Returns false on I/O error or if id == 0.
  static bool saveBookId(const char* epubPath, uint32_t bookId);

  // Load/save the last server reading_position updated_at seen for this EPUB.
  static bool loadLastSyncAt(const char* epubPath, char* out, size_t maxLen);
  static bool saveLastSyncAt(const char* epubPath, const char* updatedAt);

  // Load/save the BookFusion position associated with last_sync_at.
  static bool loadLastSyncedPosition(const char* epubPath, BookFusionStoredPosition& out);
  static bool saveLastSyncedPosition(const char* epubPath, const BookFusionStoredPosition& position);

  // Track how many milliseconds of reading time have already been pushed to
  // the BookFusion reading-time endpoint. Stored as whole seconds in the sidecar
  // under "synced_reading_s"; returned as milliseconds for direct comparison
  // with ReadingBookStats::totalReadingMs. Returns 0 if nothing has been sent yet.
  static uint64_t loadLastSyncedReadingMs(const char* epubPath);
  static bool saveLastSyncedReadingMs(const char* epubPath, uint64_t totalReadingMs);

 private:
  // Derives /.crosspoint/bookfusion_<32hexchars>.json from the epub path.
  static void buildSidecarPath(const char* epubPath, char* outPath, size_t maxLen);
};
