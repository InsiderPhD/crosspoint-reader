#include "BookFusionBookmarkPush.h"

#include <BookFusionSyncClient.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "util/BookmarkUtil.h"

namespace {
// Bookmarks store a KOReader-style xpath, "/body/DocFragment[N]/..." with N the
// 1-based spine index. Returns -1 when the xpath has no such step.
int spineIndexFromXPath(const std::string& xpath) {
  static constexpr char STEP[] = "DocFragment[";
  const size_t at = xpath.find(STEP);
  if (at == std::string::npos) return -1;
  const int oneBased = atoi(xpath.c_str() + at + sizeof(STEP) - 1);
  return oneBased >= 1 ? oneBased - 1 : -1;
}

// Inverse of Epub::calculateProgress for a known book fraction: the spine item
// holding it. Used for legacy bookmarks without an xpath.
int spineIndexFromBookFraction(const Epub& epub, const float fraction) {
  const int spineCount = epub.getSpineItemsCount();
  const float target = fraction * static_cast<float>(epub.getBookSize());
  for (int i = 0; i < spineCount; ++i) {
    if (target < static_cast<float>(epub.getCumulativeSpineItemSize(i))) return i;
  }
  return std::max(0, spineCount - 1);
}

float fractionInChapter(const Epub& epub, const int spineIndex, const float bookFraction) {
  const size_t before = spineIndex >= 1 ? epub.getCumulativeSpineItemSize(spineIndex - 1) : 0;
  const size_t chapterSize = epub.getCumulativeSpineItemSize(spineIndex) - before;
  if (chapterSize == 0) return 0.0f;
  const float offset = bookFraction * static_cast<float>(epub.getBookSize()) - static_cast<float>(before);
  return std::clamp(offset / static_cast<float>(chapterSize), 0.0f, 1.0f);
}

bool loadBookmarkFile(const std::string& path, std::vector<BookmarkEntry>& out) {
  if (!Storage.exists(path.c_str())) return false;
  const String json = Storage.readFile(path.c_str());
  return !json.isEmpty() && JsonSettingsIO::loadBookmarks(out, json.c_str());
}
}  // namespace

int BookFusionBookmarkPush::prepare(const std::shared_ptr<Epub>& epub) {
  pending.clear();
  pushedCount = 0;
  alreadySyncedCount = 0;
  fileBookmarkCount = 0;
  if (!epub) return 0;
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0 || epub->getBookSize() == 0) return 0;

  bookmarkPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  std::vector<BookmarkEntry> bookmarks;
  if (!loadBookmarkFile(bookmarkPath, bookmarks)) return 0;
  fileBookmarkCount = bookmarks.size();

  const size_t waiting = static_cast<size_t>(
      std::count_if(bookmarks.begin(), bookmarks.end(), [](const BookmarkEntry& b) { return b.bookFusionId == 0; }));
  alreadySyncedCount = static_cast<int>(bookmarks.size() - waiting);
  if (waiting == 0) return 0;
  pending.reserve(std::min(waiting, MAX_PER_SYNC));

  for (size_t i = 0; i < bookmarks.size() && pending.size() < MAX_PER_SYNC; ++i) {
    const BookmarkEntry& bookmark = bookmarks[i];
    if (bookmark.bookFusionId != 0) continue;

    int spineIndex = spineIndexFromXPath(bookmark.xpath);
    if (spineIndex < 0 || spineIndex >= spineCount) {
      spineIndex = spineIndexFromBookFraction(*epub, bookmark.percentage);
    }

    Pending item;
    item.fileIndex = static_cast<uint16_t>(i);
    item.chapterIndex = static_cast<uint16_t>(spineIndex);
    item.positionInChapter = fractionInChapter(*epub, spineIndex, bookmark.percentage);
    item.positionInBook = (static_cast<float>(spineIndex) + item.positionInChapter) / static_cast<float>(spineCount);

    // The user's own name wins; otherwise the chapter title, as the web reader does.
    if (!bookmark.name.empty()) {
      snprintf(item.title, sizeof(item.title), "%s", bookmark.name.c_str());
    } else {
      const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
      if (tocIndex >= 0) {
        snprintf(item.title, sizeof(item.title), "%s", epub->getTocItem(tocIndex).title.c_str());
      } else {
        snprintf(item.title, sizeof(item.title), "%s%d", tr(STR_SECTION_PREFIX), spineIndex + 1);
      }
    }
    pending.push_back(item);
  }
  return static_cast<int>(pending.size());
}

int BookFusionBookmarkPush::push(const uint32_t bookId) {
  if (bookId == 0) return 0;
  for (Pending& item : pending) {
    BookFusionBookmark bookmark;
    bookmark.title = item.title;
    bookmark.chapterIndex = item.chapterIndex;
    bookmark.pagePositionInChapter = item.positionInChapter;
    bookmark.pagePositionInBook = item.positionInBook;

    uint32_t remoteId = 0;
    const auto result = BookFusionSyncClient::createBookmark(bookId, bookmark, &remoteId);
    if (result == BookFusionSyncClient::OK) {
      item.remoteId = remoteId != 0 ? remoteId : BookmarkEntry::BOOKFUSION_ID_UNKNOWN;
      ++pushedCount;
      continue;
    }
    LOG_ERR("BFS", "Bookmark push failed (chapter %u): %s", static_cast<unsigned>(item.chapterIndex),
            BookFusionSyncClient::errorString(result));
    // A rejected bookmark (SERVER_ERROR) means every other one would be rejected
    // the same way, so stop on any failure rather than repeat the request.
    break;
  }
  return pushedCount;
}

void BookFusionBookmarkPush::commit() {
  if (pushedCount > 0) {
    std::vector<BookmarkEntry> bookmarks;
    // The sync blocks the reader, so the file cannot have changed since
    // prepare(); the count check only guards against a failed read.
    if (loadBookmarkFile(bookmarkPath, bookmarks) && bookmarks.size() == fileBookmarkCount) {
      for (const Pending& item : pending) {
        if (item.remoteId != 0) bookmarks[item.fileIndex].bookFusionId = item.remoteId;
      }
      if (!JsonSettingsIO::saveBookmarks(bookmarks, bookmarkPath.c_str())) {
        LOG_ERR("BFS", "Failed to persist %d BookFusion bookmark ids", pushedCount);
      }
    } else {
      LOG_ERR("BFS", "Bookmark file changed during sync; %d ids not recorded", pushedCount);
    }
  }
  std::vector<Pending>().swap(pending);
}
