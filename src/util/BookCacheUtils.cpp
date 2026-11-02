#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include "BookCacheUtils.h"
#include "activities/home/LibraryActivity.h"

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasTxtExtension(path)) {
    // Txt has no clearCache(); its cache is one directory, so drop that.
    const Txt txt(path, "/.crosspoint");
    Storage.removeDir(txt.getCachePath().c_str());
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

void noteBookInstalled(const std::string& path) {
  clearBookCache(path);
  if (!FsHelpers::hasEpubExtension(path) && !FsHelpers::hasXtcExtension(path)) return;
  // The Library never lists hidden files or folders, so they cannot make it stale.
  if (path.find("/.") != std::string::npos || (!path.empty() && path[0] == '.')) return;
  LibraryActivity::invalidateIndexCache();
}

#endif  // CROSSPOINT_SD_PLUGINS
