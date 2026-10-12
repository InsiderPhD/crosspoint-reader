#include "LibraryScan.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <deque>
#include <utility>

#include "CrossPointSettings.h"

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

// --- CLX1 index freshness marker ---------------------------------------------
//
// library.clx.dirs: the directories the CLX1 build walked, each with the FAT
// mtime it had at the time. Same validation idea as the LIDX list above, kept
// in its own file because the CLX1 format has no room for it and is shared
// byte-for-byte with CrossInk. Written to a .new file during the build and
// renamed into place only when the build succeeded, so a marker on the card
// always describes a complete index.
constexpr char CLX_MARKER_PATH[] = "/.crosspoint/library.clx.dirs";
constexpr char CLX_MARKER_NEW_PATH[] = "/.crosspoint/library.clx.dirs.new";
constexpr uint32_t CLX_MARKER_MAGIC = 0x44584C43u;  // 'C','L','X','D'
constexpr uint8_t CLX_MARKER_VERSION = 1;
// The builder skips books in folders whose path exceeds 255 bytes; directories
// themselves may nest a little deeper before the depth cap stops the walk.
constexpr uint16_t CLX_MARKER_MAX_PATH = 512;

#pragma pack(push, 1)
struct ClxMarkerHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t reserved;
  uint16_t dirCount;
  uint16_t bookCount;  // must equal the index header's: ties this marker to that file
  uint16_t reserved2;
};
struct ClxMarkerDir {
  uint32_t modified;
  uint16_t pathLen;  // path bytes follow
};
#pragma pack(pop)
static_assert(sizeof(ClxMarkerHeader) == 12, "marker header layout");
static_assert(sizeof(ClxMarkerDir) == 6, "marker record layout");

// Receives the builder's hooks during one refreshIndex() call. Lives on that
// call's stack; the builder only borrows it.
struct ClxScanContext {
  const library::BuildCallbacks* ui = nullptr;
  HalFile marker;
  uint16_t dirCount = 0;
  bool markerFailed = false;

  static bool cancelRequested(void* context) {
    const auto* self = static_cast<const ClxScanContext*>(context);
    return self->ui && self->ui->cancelRequested && self->ui->cancelRequested(self->ui->context);
  }
  static void progress(void* context, const library::BuildProgress& progress) {
    const auto* self = static_cast<const ClxScanContext*>(context);
    if (self->ui && self->ui->progress) self->ui->progress(self->ui->context, progress);
  }
  static void directoryVisited(void* context, const char* path, const uint32_t modifiedPacked) {
    auto* self = static_cast<ClxScanContext*>(context);
    if (self->markerFailed) return;
    const size_t len = strlen(path);
    if (len == 0 || len > CLX_MARKER_MAX_PATH || self->dirCount == 0xFFFFu) {
      self->markerFailed = true;
      return;
    }
    const ClxMarkerDir record{modifiedPacked, static_cast<uint16_t>(len)};
    if (self->marker.write(&record, sizeof(record)) != sizeof(record) || self->marker.write(path, len) != len) {
      self->markerFailed = true;
      return;
    }
    self->dirCount++;
  }
};

// Reads the marker's (directory, mtime) list. One entry per walked directory,
// so it scales with the folder count, not the book count; the caller holds it
// only long enough to rewrite the LIDX path list.
bool loadClxMarkerDirs(std::vector<std::pair<std::string, uint32_t>>& out) {
  out.clear();
  HalFile marker = Storage.open(CLX_MARKER_PATH);
  if (!marker) return false;
  ClxMarkerHeader header{};
  bool ok = marker.read(&header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
            header.magic == CLX_MARKER_MAGIC && header.version == CLX_MARKER_VERSION;
  if (ok) out.reserve(header.dirCount);
  for (uint16_t i = 0; ok && i < header.dirCount; i++) {
    ClxMarkerDir record{};
    ok = marker.read(&record, sizeof(record)) == static_cast<int>(sizeof(record)) && record.pathLen != 0 &&
         record.pathLen <= CLX_MARKER_MAX_PATH;
    if (!ok) break;
    std::string dirPath(record.pathLen, '\0');
    ok = marker.read(&dirPath[0], record.pathLen) == static_cast<int>(record.pathLen);
    if (ok) out.emplace_back(std::move(dirPath), record.modified);
  }
  marker.close();
  if (!ok) out.clear();
  return ok;
}

// Streams every book path out of the CLX1 index. The path list itself is what
// the caller asked for; nothing else is held (one 512-byte folder checkpoint
// table and one scratch string, both freed on return).
bool loadPathsFromClx(std::vector<std::string>& outPaths) {
  library::LibraryIndexFile index;
  if (!index.open(library::libraryIndexPath())) return false;
  const uint16_t count = index.bookCount();

  // Folder records are variable length; sparse offsets bound each path's folder
  // walk. 512 bytes, over the stack budget, so it comes from the heap.
  auto offsets = makeUniqueNoThrow<uint32_t[]>(library::LibraryIndexFile::FOLDER_CHECKPOINT_COUNT);
  uint16_t stride = 0;
  const bool haveOffsets = offsets && count > 0 && index.buildFolderCheckpoints(offsets.get(), stride);

  outPaths.clear();
  outPaths.reserve(count);
  std::string path;
  for (uint16_t ordinal = 0; ordinal < count; ordinal++) {
    library::ClixRecord record{};
    const bool ok = index.readRecord(ordinal, record) &&
                    (haveOffsets ? index.readPath(record, path, offsets.get(), stride) : index.readPath(record, path));
    if (!ok) {
      LOG_ERR(MODULE, "CLX1 path read failed at record %u", static_cast<unsigned>(ordinal));
      outPaths.clear();
      return false;
    }
    outPaths.push_back(path);
    if ((ordinal & 0x3Fu) == 0x3Fu) vTaskDelay(1);  // keep the idle task (watchdog) fed on big libraries
  }
  LOG_DBG(MODULE, "Loaded %u books from the CLX1 index", static_cast<unsigned>(count));
  return true;
}
}  // namespace

namespace LibraryScan {

bool indexEnabled() { return SETTINGS.libraryUseIndex != 0; }

const char* indexPath() { return library::libraryIndexPath(); }

bool indexIsCurrent() {
  // Storage.open, not openFileForRead: a missing marker is the normal "never
  // built" state, not an error line.
  HalFile marker = Storage.open(CLX_MARKER_PATH);
  if (!marker) return false;

  ClxMarkerHeader header{};
  if (marker.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != CLX_MARKER_MAGIC ||
      header.version != CLX_MARKER_VERSION) {
    marker.close();
    return false;
  }

  std::string dirPath;
  for (uint16_t i = 0; i < header.dirCount; i++) {
    ClxMarkerDir record{};
    if (marker.read(&record, sizeof(record)) != static_cast<int>(sizeof(record)) || record.pathLen == 0 ||
        record.pathLen > CLX_MARKER_MAX_PATH) {
      marker.close();
      return false;
    }
    dirPath.resize(record.pathLen);
    if (marker.read(&dirPath[0], record.pathLen) != static_cast<int>(record.pathLen)) {
      marker.close();
      return false;
    }
    auto dir = Storage.open(dirPath.c_str());
    if (!dir || dir.getModifyDateTimePacked() != record.modified) {
      LOG_DBG(MODULE, "CLX1 index stale: %s changed", dirPath.c_str());
      marker.close();
      return false;
    }
  }
  marker.close();

  // The marker only vouches for the index it was written with.
  library::LibraryIndexFile index;
  if (!index.open(library::libraryIndexPath())) return false;
  return index.bookCount() == header.bookCount;
}

IndexRefresh refreshIndex(const library::BuildCallbacks* ui) {
  Storage.mkdir("/.crosspoint");  // No-op if it exists; a failure surfaces in the opens below.
  // Stale from here until a build completes. The index itself stays: the
  // builder reconciles against it and keeps it when the build does not finish.
  Storage.remove(CLX_MARKER_PATH);
  Storage.remove(CLX_MARKER_NEW_PATH);

  ClxScanContext scan;
  scan.ui = ui;
  ClxMarkerHeader header{CLX_MARKER_MAGIC, CLX_MARKER_VERSION, 0, 0, 0, 0};
  if (!Storage.openFileForWrite(MODULE, CLX_MARKER_NEW_PATH, scan.marker) ||
      scan.marker.write(&header, sizeof(header)) != sizeof(header)) {
    // The index can still be built and read; it just rescans on the next visit.
    LOG_ERR(MODULE, "Cannot write the CLX1 directory marker");
    scan.markerFailed = true;
  }

  library::BuildCallbacks callbacks;
  callbacks.context = &scan;
  callbacks.cancelRequested = &ClxScanContext::cancelRequested;
  callbacks.progress = &ClxScanContext::progress;
  callbacks.directoryVisited = &ClxScanContext::directoryVisited;

  library::BuildStats stats;
  const uint32_t startMs = millis();
  const bool ok = library::buildLibraryIndex("/", stats, /*readMetadata=*/true, &callbacks);
  LOG_INF(MODULE, "CLX1 scan %s: %u books, %u parsed, %u reused, %u resumed, %lu ms", ok ? "ok" : "not installed",
          static_cast<unsigned>(stats.books), static_cast<unsigned>(stats.parsed),
          static_cast<unsigned>(stats.metadataReused), static_cast<unsigned>(stats.metadataCached),
          static_cast<unsigned long>(millis() - startMs));

  bool markerWritten = false;
  if (scan.marker) {
    if (ok && !stats.retryAdvised && !scan.markerFailed) {
      header.dirCount = scan.dirCount;
      header.bookCount = stats.books;
      markerWritten = scan.marker.seekSet(0) && scan.marker.write(&header, sizeof(header)) == sizeof(header);
    }
    markerWritten = scan.marker.close() && markerWritten;
  }
  if (!markerWritten || !Storage.rename(CLX_MARKER_NEW_PATH, CLX_MARKER_PATH)) {
    Storage.remove(CLX_MARKER_NEW_PATH);
    if (ok) LOG_INF(MODULE, "CLX1 index built without a marker; it will be rescanned on the next visit");
  }

  if (ok) return IndexRefresh::Ok;
  switch (stats.failure) {
    case library::BuildFailure::Cancelled:
      return IndexRefresh::Cancelled;
    case library::BuildFailure::TooManyBooks:
      return IndexRefresh::TooManyBooks;
    case library::BuildFailure::None:
    case library::BuildFailure::Error:
      break;
  }
  return IndexRefresh::Failed;
}

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
  // No current path list, but a current CLX1 index knows every book: stream the
  // paths out of it instead of walking the card, then persist them as the path
  // list (with the directory mtimes the CLX1 build recorded) so the next call
  // is the plain sequential load above.
  if (indexEnabled() && indexIsCurrent() && loadPathsFromClx(outPaths)) {
    std::vector<std::pair<std::string, uint32_t>> dirMtimes;
    if (loadClxMarkerDirs(dirMtimes)) saveToCache(outPaths, dirMtimes);
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
  // The CLX1 index is invalidated through its marker only. library.clx stays so
  // the next scan reuses every unchanged book's metadata instead of re-reading
  // each EPUB. Done whatever the setting says: a marker left behind while the
  // index is switched off would vouch for a stale index when it is switched on.
  if (Storage.exists(CLX_MARKER_PATH)) {
    Storage.remove(CLX_MARKER_PATH);
    LOG_DBG(MODULE, "Invalidated CLX1 index marker");
  }
}

}  // namespace LibraryScan
