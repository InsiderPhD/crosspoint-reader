#include "LibraryScan.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <deque>
#include <utility>

namespace {
constexpr char MODULE[] = "LIBSCAN";

// Library index cache — persists the enumerated book list across activity
// re-creations. Format owned here; bump the version on any layout change.
constexpr char LIBRARY_INDEX_PATH[] = "/.crosspoint/library_index.bin";
constexpr uint32_t LIBRARY_INDEX_MAGIC = 0x4C494458u;  // 'L','I','D','X'
constexpr uint8_t LIBRARY_INDEX_VERSION = 1;
// Maximum allowed book path length in the cache. Guards against corrupt length
// fields causing a runaway heap allocation in readSafePath().
constexpr uint32_t MAX_CACHE_PATH_LEN = 512;

// Reads a serialization::writeString-format string (uint32_t len + raw bytes)
// but clamps the length before any allocation so a corrupt cache file can't
// cause an OOM. Returns false if the length is out of range.
bool readSafePath(HalFile& f, std::string& s) {
  uint32_t len = 0;
  serialization::readPod(f, len);
  if (len > MAX_CACHE_PATH_LEN) return false;
  s.resize(len);
  if (len > 0 && f.read(&s[0], len) != static_cast<int>(len)) return false;
  return true;
}

// Fast path: load the cached book list. Returns false (and leaves outPaths
// cleared) if the cache is missing, corrupt, or any scanned directory's mtime
// has changed since it was written.
bool tryLoadFromCache(std::vector<std::string>& outPaths) {
  HalFile f;
  if (!Storage.openFileForRead(MODULE, LIBRARY_INDEX_PATH, f)) return false;

  uint32_t magic = 0;
  uint8_t version = 0;
  serialization::readPod(f, magic);
  serialization::readPod(f, version);
  if (magic != LIBRARY_INDEX_MAGIC || version != LIBRARY_INDEX_VERSION) {
    f.close();
    return false;
  }

  // Validate each stored directory's mtime against the SD card now.
  // Any change (file added, removed, moved within that dir) updates the FAT
  // directory timestamp and causes a full re-scan on the next open.
  uint16_t dirCount = 0;
  serialization::readPod(f, dirCount);
  for (uint16_t i = 0; i < dirCount; ++i) {
    uint32_t storedMtime = 0;
    std::string dirPath;
    serialization::readPod(f, storedMtime);
    if (!readSafePath(f, dirPath)) {
      f.close();
      return false;
    }

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || dir.getModifyDateTimePacked() != storedMtime) {
      f.close();
      return false;
    }
  }

  uint16_t bookCount = 0;
  serialization::readPod(f, bookCount);
  outPaths.clear();
  outPaths.reserve(bookCount);
  for (uint16_t i = 0; i < bookCount; ++i) {
    std::string path;
    if (!readSafePath(f, path)) {
      f.close();
      outPaths.clear();
      return false;
    }
    outPaths.push_back(std::move(path));
  }

  f.close();
  LOG_DBG(MODULE, "Loaded %u books from library index cache", bookCount);
  return true;
}

void saveToCache(const std::vector<std::string>& bookPaths,
                 const std::vector<std::pair<std::string, uint32_t>>& dirMtimes) {
  Storage.mkdir("/.crosspoint");  // No-op if already exists; failure is caught below.

  HalFile f;
  if (!Storage.openFileForWrite(MODULE, LIBRARY_INDEX_PATH, f)) {
    LOG_ERR(MODULE, "Failed to write library index cache");
    return;
  }

  serialization::writePod(f, LIBRARY_INDEX_MAGIC);
  serialization::writePod(f, LIBRARY_INDEX_VERSION);

  const uint16_t dirCount = static_cast<uint16_t>(std::min<size_t>(dirMtimes.size(), 0xFFFFu));
  serialization::writePod(f, dirCount);
  for (uint16_t i = 0; i < dirCount; ++i) {
    serialization::writePod(f, dirMtimes[i].second);    // mtime
    serialization::writeString(f, dirMtimes[i].first);  // path
  }

  const uint16_t bookCount = static_cast<uint16_t>(std::min<size_t>(bookPaths.size(), 0xFFFFu));
  serialization::writePod(f, bookCount);
  for (uint16_t i = 0; i < bookCount; ++i) {
    serialization::writeString(f, bookPaths[i]);
  }

  f.close();
  LOG_DBG(MODULE, "Saved library index cache: %u dirs, %u books", dirCount, bookCount);
}
}  // namespace

namespace LibraryScan {

void enumerateBooks(std::vector<std::string>& outPaths) {
  // TEMP diagnostic (crash triage): heap state on entering a browse. Remove once
  // the browse crash is root-caused.
  LOG_DBG("MEMDIAG", "enumerateBooks entry: free=%u largest=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  // Fast path: try to load the cached book list. Falls back to BFS if the
  // cache is missing, corrupt, or any scanned directory's mtime has changed.
  if (tryLoadFromCache(outPaths)) {
    LOG_DBG("MEMDIAG", "enumerateBooks cache-hit: %zu books", outPaths.size());
    return;
  }
  LOG_DBG("MEMDIAG", "enumerateBooks cache-miss: full BFS re-scan");

  outPaths.clear();
  outPaths.reserve(64);  // Conservative initial guess; std::vector grows as needed.

  // BFS via deque to keep stack usage bounded regardless of folder nesting depth.
  std::deque<std::string> dirsToScan;
  dirsToScan.emplace_back("/");

  // Collect (dirPath, mtime) pairs so saveToCache() can validate the cache on
  // the next open. One entry per directory actually opened during this BFS.
  std::vector<std::pair<std::string, uint32_t>> dirMtimes;

  // Stack buffer for filename reads. Matches FileBrowserActivity::loadFiles
  // which uses 500 bytes — long FAT filenames can exceed 255 chars in some cases.
  char name[500];

  while (!dirsToScan.empty()) {
    std::string dirPath = std::move(dirsToScan.front());
    dirsToScan.pop_front();

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) continue;
    // Record mtime before rewindDirectory() since the seek may affect internal state.
    dirMtimes.emplace_back(dirPath, dir.getModifyDateTimePacked());
    dir.rewindDirectory();

    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      file.getName(name, sizeof(name));
      // Skip hidden entries and Windows-managed metadata folders. Notably this
      // skips .crosspoint itself, which would otherwise be recursed into.
      if (name[0] == '.' || std::strcmp(name, "System Volume Information") == 0) continue;

      std::string fullPath = dirPath;
      if (fullPath.back() != '/') fullPath.push_back('/');
      fullPath += name;

      if (file.isDirectory()) {
        dirsToScan.push_back(std::move(fullPath));
      } else {
        const std::string_view fn{name};
        if (FsHelpers::hasEpubExtension(fn) || FsHelpers::hasXtcExtension(fn)) {
          outPaths.push_back(std::move(fullPath));
        }
      }
    }
  }

  LOG_DBG(MODULE, "Enumerated %zu books from SD", outPaths.size());
  saveToCache(outPaths, dirMtimes);
}

void invalidateIndex() {
  if (Storage.exists(LIBRARY_INDEX_PATH)) {
    Storage.remove(LIBRARY_INDEX_PATH);
    LOG_DBG(MODULE, "Invalidated library index cache");
  }
}

}  // namespace LibraryScan
