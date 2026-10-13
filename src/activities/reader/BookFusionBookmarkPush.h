#pragma once
#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookmarkEntry.h"

/**
 * Pushes the open book's bookmarks to BookFusion, each one once.
 *
 * Split in three so the SD card and the book metadata are only touched outside
 * the TLS window (which lends wolfSSL the framebuffer and runs on a tight heap):
 *   prepare() - before TLS: reads the bookmark file, keeps a small fixed record
 *               per bookmark BookFusion hasn't got.
 *   push()    - inside TLS, on the caller's open connection: network only.
 *   commit()  - after TLS: writes the returned ids into the bookmark file.
 */
class BookFusionBookmarkPush {
 public:
  // Returns the number of bookmarks waiting to be pushed.
  int prepare(const std::shared_ptr<Epub>& epub);
  // Returns the number created. Stops at the first auth or transport failure;
  // the rest stay pending for the next sync.
  int push(uint32_t bookId);
  void commit();
  // Bookmarks of this book that BookFusion has, counting this sync's pushes.
  int syncedTotal() const { return alreadySyncedCount + pushedCount; }
  // Every bookmark saved for this book on the device (valid after prepare()).
  int localTotal() const { return static_cast<int>(fileBookmarkCount); }

 private:
  // Bounds the heap held across the TLS session; later bookmarks go next sync.
  static constexpr size_t MAX_PER_SYNC = 16;

  struct Pending {
    uint16_t fileIndex = 0;
    uint16_t chapterIndex = 0;
    float positionInChapter = 0.0f;
    float positionInBook = 0.0f;
    uint32_t remoteId = 0;  // 0 until pushed
    char title[BookmarkEntry::MAX_NAME_LENGTH + 1] = {};
  };

  std::string bookmarkPath;
  std::vector<Pending> pending;
  size_t fileBookmarkCount = 0;
  int alreadySyncedCount = 0;
  int pushedCount = 0;
};
