#include "BookMoveUtils.h"

#include <BookFusionBookIdStore.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <HardcoverBookStore.h>
#include <KOReaderSyncStateStore.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include "ClippingStore.h"
#include "CrossPointState.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "activities/home/LibraryScan.h"
#include "util/BookmarkUtil.h"

namespace {
constexpr char CACHE_DIR[] = "/.crosspoint";

enum class BookType { None, Epub, Xtc, Txt };

BookType bookTypeForPath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) return BookType::Epub;
  if (FsHelpers::hasXtcExtension(path)) return BookType::Xtc;
  // The reader opens Markdown through the TXT reader, so it shares the TXT cache.
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) return BookType::Txt;
  return BookType::None;
}

std::string cachePathFor(const std::string& path, const BookType type) {
  switch (type) {
    case BookType::Epub:
      return Epub::cachePathFor(path, CACHE_DIR);
    case BookType::Xtc:
      return Xtc(path, CACHE_DIR).getCachePath();
    case BookType::Txt:
      return Txt(path, CACHE_DIR).getCachePath();
    case BookType::None:
      break;
  }
  return "";
}

std::string_view fileNameOf(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return (slash == std::string::npos) ? std::string_view(path) : std::string_view(path).substr(slash + 1);
}

bool isDirectory(const std::string& path) {
  auto file = Storage.open(path.c_str());
  if (!file) return false;
  const bool dir = file.isDirectory();
  file.close();
  return dir;
}
}  // namespace

namespace BookMoveUtils {

RenameResult renameFile(const std::string& oldPath, const std::string& newPath) {
  const BookType type = bookTypeForPath(oldPath);
  if (!FsHelpers::isValidFileName(fileNameOf(newPath)) || bookTypeForPath(newPath) != type) {
    LOG_ERR("MOVE", "Invalid rename target: %s", newPath.c_str());
    return RenameResult::InvalidName;
  }
  if (oldPath == newPath) return RenameResult::Ok;
  if (isDirectory(oldPath)) {
    LOG_ERR("MOVE", "Refusing to rename a folder: %s", oldPath.c_str());
    return RenameResult::InvalidName;
  }
  // FAT names are case-insensitive, so this also refuses a case-only change of
  // the file's own name; SdFat's rename would reject that anyway.
  if (Storage.exists(newPath.c_str())) {
    LOG_ERR("MOVE", "Rename target already exists: %s", newPath.c_str());
    return RenameResult::TargetExists;
  }

  if (type == BookType::None) {
    if (!Storage.rename(oldPath.c_str(), newPath.c_str())) {
      LOG_ERR("MOVE", "Failed to rename %s -> %s", oldPath.c_str(), newPath.c_str());
      return RenameResult::Failed;
    }
    return RenameResult::Ok;
  }

  const std::string oldCachePath = cachePathFor(oldPath, type);
  const std::string newCachePath = cachePathFor(newPath, type);

  // Nothing is at newPath, so state already keyed to it was left behind by a
  // book that has since been deleted or moved away. Drop it rather than refuse
  // the name: it can never be reached again, and it would block the rename below.
  if (Storage.exists(newCachePath.c_str())) {
    LOG_INF("MOVE", "Dropping orphaned cache at %s", newCachePath.c_str());
    Storage.removeDir(newCachePath.c_str());
  }

  bool cacheMoved = false;
  if (Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("MOVE", "Failed to move cache %s -> %s", oldCachePath.c_str(), newCachePath.c_str());
      return RenameResult::Failed;
    }
    cacheMoved = true;
  }

  if (!Storage.rename(oldPath.c_str(), newPath.c_str())) {
    LOG_ERR("MOVE", "Failed to rename %s -> %s", oldPath.c_str(), newPath.c_str());
    if (cacheMoved && !Storage.rename(newCachePath.c_str(), oldCachePath.c_str())) {
      LOG_ERR("MOVE", "Failed to move cache back %s -> %s", newCachePath.c_str(), oldCachePath.c_str());
    }
    return RenameResult::Failed;
  }

  // From here the book lives at newPath. Everything below follows it; a failure
  // is logged and the rest still runs.
  if (type == BookType::Epub) {
    const std::string oldBookmarks = BookmarkUtil::getBookmarkPath(oldPath);
    const std::string newBookmarks = BookmarkUtil::getBookmarkPath(newPath);
    if (oldBookmarks != newBookmarks && Storage.exists(oldBookmarks.c_str())) {
      if (Storage.exists(newBookmarks.c_str())) Storage.remove(newBookmarks.c_str());  // orphan, as above
      if (!Storage.rename(oldBookmarks.c_str(), newBookmarks.c_str())) {
        LOG_ERR("MOVE", "Failed to move bookmarks %s -> %s", oldBookmarks.c_str(), newBookmarks.c_str());
      }
    }
    if (!ClippingStore::moveForFilePath(oldPath, newPath, "epub")) {
      LOG_ERR("MOVE", "Failed to move clippings for %s", newPath.c_str());
    }
    BookFusionBookIdStore::movePath(oldPath.c_str(), newPath.c_str());
    KOReaderSyncStateStore::movePath(oldPath.c_str(), newPath.c_str());
  }
  HardcoverBookStore::movePath(oldPath.c_str(), newPath.c_str());

  if (!RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath)) {
    LOG_ERR("MOVE", "Failed to save recents after rename: %s", newPath.c_str());
  }
  // Stats are keyed by a content hash, so they would re-attach on the next open
  // anyway; this just keeps the stored path openable from the stats screens
  // until then. False only means the book has no stats yet.
  READING_STATS.updateBookPath(oldPath, newPath);

  bool stateChanged = false;
  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    stateChanged = true;
  }
  if (APP_STATE.readerTimeLeftBookPath == oldPath) {
    APP_STATE.readerTimeLeftBookPath = newPath;
    stateChanged = true;
  }
  if (stateChanged && !APP_STATE.saveToFile()) {
    LOG_ERR("MOVE", "Failed to save renamed open-book path: %s", newPath.c_str());
  }

  // The library index lists books by path and keys off the SD root's mtime,
  // which a rename inside a subfolder does not touch.
  if (type == BookType::Epub || type == BookType::Xtc) LibraryScan::invalidateIndex();

  LOG_INF("MOVE", "Renamed %s -> %s", oldPath.c_str(), newPath.c_str());
  return RenameResult::Ok;
}

}  // namespace BookMoveUtils
