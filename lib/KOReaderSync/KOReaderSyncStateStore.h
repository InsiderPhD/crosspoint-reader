#pragma once

#include <cstddef>
#include <cstdint>

struct KOReaderStoredPosition {
  float percentage = 0.0f;
  int spineIndex = 0;
  int pageNumber = -1;
  int totalPages = 0;
};

/**
 * Per-book sidecar state for KOReader quick sync conflict resolution.
 *
 * File path: /.crosspoint/koreader_sync_<md5_of_epub_path>.json
 */
class KOReaderSyncStateStore {
 public:
  // Load/save the last sync timestamp used for conflict decisions.
  static bool loadLastSyncTimestamp(const char* epubPath, int64_t& outTimestamp);
  static bool saveLastSyncTimestamp(const char* epubPath, int64_t timestamp);

  // Load/save the position associated with the most recent successful sync.
  static bool loadLastSyncedPosition(const char* epubPath, KOReaderStoredPosition& out);
  static bool saveLastSyncedPosition(const char* epubPath, const KOReaderStoredPosition& position);

  // The EPUB was renamed or moved: re-key its sidecar (the file name is a hash
  // of the path) so the sync state follows it. True when there was nothing to move.
  static bool movePath(const char* oldEpubPath, const char* newEpubPath);

 private:
  static void buildSidecarPath(const char* epubPath, char* outPath, size_t maxLen);
};
