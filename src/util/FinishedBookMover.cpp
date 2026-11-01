#include "FinishedBookMover.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "BookIdentity.h"
#include "BookMoveUtils.h"
#include "CrossPointSettings.h"
#include "ReadingStatsStore.h"

namespace {

// Fixed names, deliberately not tr(): the folder lives on the SD card and must not
// change when the UI language does.
const char* folderFor(const uint8_t setting) {
  switch (setting) {
    case CrossPointSettings::FINISHED_FOLDER_READ:
      return "/Read";
    case CrossPointSettings::FINISHED_FOLDER_COMPLETED:
      return "/Completed";
    default:
      return nullptr;
  }
}

}  // namespace

namespace FinishedBookMover {

bool moveIfFinished(const std::string& bookPath) {
  const char* folder = folderFor(SETTINGS.finishedBookFolder);
  if (folder == nullptr || bookPath.empty()) {
    return false;
  }

  const auto& snapshot = READING_STATS.getLastSessionSnapshot();
  if (!snapshot.valid || !snapshot.completedThisSession || snapshot.path != BookIdentity::normalizePath(bookPath)) {
    return false;
  }

  const size_t folderLen = strlen(folder);
  if (bookPath.compare(0, folderLen, folder) == 0 && bookPath.size() > folderLen && bookPath[folderLen] == '/') {
    return false;  // already filed
  }

  const size_t slash = bookPath.find_last_of('/');
  const std::string newPath = std::string(folder) + "/" + bookPath.substr(slash == std::string::npos ? 0 : slash + 1);

  if (!Storage.ensureDirectoryExists(folder)) {
    LOG_ERR("FBM", "Cannot create %s", folder);
    return false;
  }

  // renameFile carries the cache, bookmarks, clippings, sync sidecars, recents,
  // stats and APP_STATE across, and refuses an occupied target.
  const auto result = BookMoveUtils::renameFile(bookPath, newPath);
  if (result != BookMoveUtils::RenameResult::Ok) {
    LOG_ERR("FBM", "Not moving %s -> %s (result %d)", bookPath.c_str(), newPath.c_str(), static_cast<int>(result));
    return false;
  }
  LOG_INF("FBM", "Moved finished book %s -> %s", bookPath.c_str(), newPath.c_str());
  return true;
}

}  // namespace FinishedBookMover
