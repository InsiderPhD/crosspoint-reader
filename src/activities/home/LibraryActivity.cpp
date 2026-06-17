#include "LibraryActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <utility>

#include "../util/ConfirmationActivity.h"
#include "BookDetailsActivity.h"
#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/bookfusion24.h"
#include "components/icons/cover.h"
#include "fontIds.h"

namespace {
constexpr char MODULE[] = "LIBRARY";
// Tile-internal padding shared with Lyra3CoversTheme.cpp so the selection
// styling and inner cover position match pixel-for-pixel.
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int rowVGap = 16;
constexpr int pageIndicatorHeight = 30;

// Library index cache — persists bookPaths across activity re-creations.
constexpr char LIBRARY_INDEX_PATH[] = "/.crosspoint/library_index.bin";
constexpr uint32_t LIBRARY_INDEX_MAGIC = 0x4C494458u;  // 'L','I','D','X'
constexpr uint8_t LIBRARY_INDEX_VERSION = 1;
// Maximum allowed book path length in the cache. Guards against corrupt length
// fields causing a runaway heap allocation in readSafePath().
constexpr uint32_t MAX_CACHE_PATH_LEN = 512;

// Reads a serialization::writeString-format string (uint32_t len + raw bytes)
// but clamps the length before any allocation so a corrupt cache file can't
// cause an OOM. Returns false if the length is out of range.
static bool readSafePath(HalFile& f, std::string& s) {
  uint32_t len = 0;
  serialization::readPod(f, len);
  if (len > MAX_CACHE_PATH_LEN) return false;
  s.resize(len);
  if (len > 0 && f.read(&s[0], len) != static_cast<int>(len)) return false;
  return true;
}

struct GridLayout {
  int sidePadding;
  int tileWidth;
  int coverDrawW;
  int coverDrawH;
  int rowH;
  int rows;
  int topY;  // Y at which the grid begins, below the header bar.
};

// Helper: compute the grid layout based on screen dimensions + active row count.
// Cover height is capped at 226 (Lyra3Covers's homeCoverHeight) so the thumb
// cache (thumb_226.bmp) renders at its native height without scaling. The
// caller passes the total bottom-reserved area (page indicator strip + button
// hints bar) so both can be drawn beneath the grid without overlap.
inline GridLayout computeLayout(int screenW, int screenH, int sidePadding, int gridTopY, int reservedBottom, int rows) {
  GridLayout L;
  L.sidePadding = sidePadding;
  L.tileWidth = (screenW - 2 * L.sidePadding) / LibraryActivity::GRID_COLS;
  L.coverDrawW = L.tileWidth - 2 * hPaddingInSelection;
  L.rows = rows;
  L.topY = gridTopY;

  const int vGaps = (rows - 1) * rowVGap;
  L.rowH = (screenH - L.topY - vGaps - reservedBottom) / rows;

  // Room below the cover for: title (up to 2 lines) + author (1 line) + progress (1 line)
  // + selection padding. Sized to match Lyra3Covers' info layout per tile.
  constexpr int titleAreaApprox = 90;
  L.coverDrawH = std::min(L.rowH - titleAreaApprox - 2 * hPaddingInSelection, LibraryActivity::COVER_HEIGHT);
  return L;
}

// Compute (tileX, tileY) for a given slot index in the page.
inline void tileOrigin(int slot, const GridLayout& L, int& outX, int& outY) {
  const int row = slot / LibraryActivity::GRID_COLS;
  const int col = slot % LibraryActivity::GRID_COLS;
  outX = L.sidePadding + col * L.tileWidth;
  outY = L.topY + row * (L.rowH + rowVGap);
}
}  // namespace

int LibraryActivity::gridRows() const {
  // Landscape only fits a single row of Lyra3Covers-sized covers (2 × 226 +
  // title + chrome > 480). Detect via screen aspect.
  return (renderer.getScreenWidth() > renderer.getScreenHeight()) ? 1 : 2;
}

void LibraryActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  lastRenderedPage = static_cast<size_t>(-1);
  pageRendered = false;
  pageBufferStored = false;
  // Pull last-chosen sort from settings (shared across all list activities).
  currentSort =
      (SETTINGS.sortMode < SORT_MODE_COUNT) ? static_cast<SortMode>(SETTINGS.sortMode) : SortMode::AlphabeticAsc;
  // Defer the SD BFS to the first render so we can paint a "Loading…" popup
  // before it starts. Matches the pendingSortRebuild pattern below.
  initialLoadPending = true;
  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();
  freePageBuffer();
  bookPaths.clear();
  bookPaths.shrink_to_fit();
  sortedIndices.clear();
  sortedIndices.shrink_to_fit();
  authorCache.clear();
  authorCache.shrink_to_fit();
  dateAddedCache.clear();
  dateAddedCache.shrink_to_fit();
  bfBadgeCache.clear();
  bfBadgeCacheReady = false;
  authorCacheReady = false;
  dateAddedCacheReady = false;
  pendingSortRebuild = false;
}

bool LibraryActivity::tryLoadFromCache() {
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
    if (!readSafePath(f, dirPath)) { f.close(); return false; }

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || dir.getModifyDateTimePacked() != storedMtime) {
      f.close();
      return false;
    }
  }

  uint16_t bookCount = 0;
  serialization::readPod(f, bookCount);
  bookPaths.clear();
  bookPaths.reserve(bookCount);
  for (uint16_t i = 0; i < bookCount; ++i) {
    std::string path;
    if (!readSafePath(f, path)) { f.close(); bookPaths.clear(); return false; }
    bookPaths.push_back(std::move(path));
  }

  f.close();
  LOG_DBG(MODULE, "Loaded %u books from library index cache", bookCount);
  return true;
}

void LibraryActivity::saveToCache(const std::vector<std::pair<std::string, uint32_t>>& dirMtimes) {
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
    serialization::writePod(f, dirMtimes[i].second);  // mtime
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

void LibraryActivity::enumerateBooks() {
  // Fast path: try to load the cached book list. Falls back to BFS if the
  // cache is missing, corrupt, or any scanned directory's mtime has changed.
  if (tryLoadFromCache()) {
    authorCache.assign(bookPaths.size(), std::string{});
    dateAddedCache.assign(bookPaths.size(), 0u);
    bfBadgeCache.assign(bookPaths.size(), false);
    authorCacheReady = false;
    dateAddedCacheReady = false;
    bfBadgeCacheReady = false;
    rebuildSortedIndices();
    return;
  }

  bookPaths.clear();
  bookPaths.reserve(64);  // Conservative initial guess; std::vector grows as needed.

  // BFS via deque to keep stack usage bounded regardless of folder nesting depth.
  std::deque<std::string> dirsToScan;
  dirsToScan.emplace_back("/");

  // Collect (dirPath, mtime) pairs so saveToCache() can validate the cache on
  // the next open. One entry per directory actually opened during this BFS.
  std::vector<std::pair<std::string, uint32_t>> dirMtimes;

  // Stack buffer for filename reads. Matches FileBrowserActivity::loadFiles (FileBrowserActivity.cpp:86)
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
          bookPaths.push_back(std::move(fullPath));
        }
      }
    }
  }

  LOG_DBG(MODULE, "Enumerated %zu books from SD", bookPaths.size());

  // Reset caches; their contents are bookPaths-indexed and must match the new list.
  authorCache.assign(bookPaths.size(), std::string{});
  dateAddedCache.assign(bookPaths.size(), 0u);
  bfBadgeCache.assign(bookPaths.size(), false);
  authorCacheReady = false;
  dateAddedCacheReady = false;
  bfBadgeCacheReady = false;

  rebuildSortedIndices();
  saveToCache(dirMtimes);
}

namespace {
// Helper: return the filename (without extension) view into `path`. Lifetime is bound
// to the caller-owned string.
std::string_view filenameView(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
  const auto dot = path.find_last_of('.');
  const size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;
  return std::string_view(path).substr(start, end - start);
}
}  // namespace

void LibraryActivity::rebuildSortedIndices() {
  const bool needsAuthor =
      (currentSort == SortMode::AuthorAsc || currentSort == SortMode::AuthorDesc) && !authorCacheReady;
  const bool needsDateAdded =
      (currentSort == SortMode::DateAddedNewest || currentSort == SortMode::DateAddedOldest) && !dateAddedCacheReady;

  if (needsAuthor) {
    for (size_t i = 0; i < bookPaths.size(); i++) {
      const std::string& path = bookPaths[i];
      if (FsHelpers::hasEpubExtension(path)) {
        Epub epub(path, "/.crosspoint");
        if (Storage.exists((epub.getCachePath() + "/book.bin").c_str()) && epub.load(true, true)) {
          authorCache[i] = epub.getAuthor();
        }
      } else if (FsHelpers::hasXtcExtension(path)) {
        Xtc xtc(path, "/.crosspoint");
        if (xtc.load()) authorCache[i] = xtc.getAuthor();
      }
    }
    authorCacheReady = true;
  }

  if (needsDateAdded) {
    for (size_t i = 0; i < bookPaths.size(); i++) {
      HalFile f;
      if (Storage.openFileForRead(MODULE, bookPaths[i], f)) {
        dateAddedCache[i] = f.getModifyDateTimePacked();
        f.close();
      }
    }
    dateAddedCacheReady = true;
  }

  const bool needsBfBadge =
      (currentSort == SortMode::BookFusionFirst || currentSort == SortMode::BookFusionLast) && !bfBadgeCacheReady;
  if (needsBfBadge) {
    for (size_t i = 0; i < bookPaths.size(); i++) {
      bfBadgeCache[i] = FsHelpers::hasEpubExtension(bookPaths[i]) && BookFusionBookIdStore::hasBookId(bookPaths[i].c_str());
    }
    bfBadgeCacheReady = true;
  }

  // RecentBooksStore is capped at 10 entries (RecentBooksStore.cpp:18) — linear lookup is fine.
  const auto& recents = RECENT_BOOKS.getBooks();

  std::vector<SortEntry> entries;
  entries.reserve(bookPaths.size());
  for (size_t i = 0; i < bookPaths.size(); i++) {
    SortEntry e;
    e.sortKey = filenameView(bookPaths[i]);
    e.authorKey = authorCacheReady ? std::string_view(authorCache[i]) : std::string_view{};
    e.dateAddedTs = dateAddedCacheReady ? dateAddedCache[i] : 0u;
    e.hasBfBadge = bfBadgeCacheReady ? bfBadgeCache[i] : false;
    e.progressPercent = -1;
    e.lastOpenedRank = LAST_OPENED_NEVER;
    for (size_t r = 0; r < recents.size(); r++) {
      if (recents[r].path == bookPaths[i]) {
        e.progressPercent = recents[r].progressPercent;
        e.lastOpenedRank = static_cast<uint16_t>(r);
        break;
      }
    }
    entries.push_back(e);
  }

  applySort(sortedIndices, entries, currentSort);
}

std::string LibraryActivity::pathAtLogicalIndex(size_t logicalIdx) const {
  if (logicalIdx >= sortedIndices.size()) return {};
  return bookPaths[sortedIndices[logicalIdx]];
}

std::string LibraryActivity::currentPath() const { return pathAtLogicalIndex(selectorIndex); }

LibraryActivity::SlotRect LibraryActivity::slotRect(int slotIndexInPage) const {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Header sits at metrics.topPadding for headerHeight pixels; grid starts below
  // it after one verticalSpacing of breathing room — matches RecentBooksActivity.cpp:199-202.
  const int gridTopY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const GridLayout L = computeLayout(screenW, screenH, metrics.contentSidePadding, gridTopY,
                                     pageIndicatorHeight + metrics.buttonHintsHeight, gridRows());

  int tileX, tileY;
  tileOrigin(slotIndexInPage, L, tileX, tileY);

  SlotRect r;
  r.x = tileX + hPaddingInSelection;
  r.y = tileY + hPaddingInSelection;
  r.width = L.coverDrawW;
  r.height = L.coverDrawH;
  return r;
}

bool LibraryActivity::storePageBuffer() {
  uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) return false;
  freePageBuffer();
  const size_t bufferSize = renderer.getBufferSize();
  pageBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!pageBuffer) {
    LOG_ERR(MODULE, "Failed to malloc %zu byte page buffer", bufferSize);
    return false;
  }
  std::memcpy(pageBuffer, fb, bufferSize);
  return true;
}

bool LibraryActivity::restorePageBuffer() {
  if (!pageBuffer) return false;
  uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) return false;
  std::memcpy(fb, pageBuffer, renderer.getBufferSize());
  return true;
}

void LibraryActivity::freePageBuffer() {
  if (pageBuffer) {
    free(pageBuffer);
    pageBuffer = nullptr;
  }
  pageBufferStored = false;
  pageRendered = false;
}

void LibraryActivity::render(RenderLock&&) {
  // Initial entry: paint a "Loading…" popup over whatever the prior activity
  // left in the framebuffer, push it to e-ink, THEN run the (synchronous) SD
  // BFS. Without this, the device looks frozen for a second or two on a
  // populated library. Falls through to the normal render path so the
  // populated page paints in the same render call.
  if (initialLoadPending) {
    GUI.drawPopup(renderer, tr(STR_LOADING));
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    if (SETTINGS.darkMode) renderer.invertScreen();

    enumerateBooks();
    initialLoadPending = false;
    // Fall through.
  }

  // A sort mode change can require a metadata pass (book.bin parsing, SD stats)
  // that takes seconds for large libraries. Show a "Sorting…" popup *before*
  // running the rebuild so the user sees feedback rather than a frozen modal.
  if (pendingSortRebuild) {
    GUI.drawPopup(renderer, tr(STR_SORTING));
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    if (SETTINGS.darkMode) renderer.invertScreen();

    rebuildSortedIndices();
    pendingSortRebuild = false;
    lastRenderedPage = static_cast<size_t>(-1);
    pageBufferStored = false;
    // Fall through to the normal render path so the new sorted page is drawn.
  }

  if (bookPaths.empty()) {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   tr(STR_LIBRARY), sortModeLabel(currentSort));
    drawButtonHints();
    contextMenu.render(renderer);
    sortMenu.render(renderer);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  const size_t page = currentPage();
  const bool pageChanged = (page != lastRenderedPage) || !pageBufferStored;

  if (pageChanged) {
    renderPageFromScratch();
    lastRenderedPage = page;
  } else {
    renderSelectionOnly();
  }

  drawButtonHints();
  contextMenu.render(renderer);
  sortMenu.render(renderer);
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

void LibraryActivity::drawButtonHints() {
  // Same label set as HomeActivity (HomeActivity.cpp:407-409). When the book
  // options modal is open, btn1 shows Back to cancel; otherwise it's empty
  // because short-press Back goes home (long-press isn't yet bound to a label).
  const auto labels = contextMenu.isOpen()
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LibraryActivity::refreshCurrentPageMeta() {
  const size_t page = currentPage();
  const size_t pageStart = page * pageSize();
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookPaths.size() - pageStart));

  // Reset all slots so stale data from a previous page doesn't leak.
  for (auto& m : currentPageMeta) {
    m.title.clear();
    m.author.clear();
    m.progressPercent = -1;
    m.hasCover = false;
    m.hasBfBadge = false;
    m.thumbPath.clear();
  }

  for (int i = 0; i < booksOnPage; ++i) {
    const std::string path = pathAtLogicalIndex(pageStart + i);
    auto& meta = currentPageMeta[i];

    if (FsHelpers::hasEpubExtension(path)) {
      Epub epub(path, "/.crosspoint");
      meta.thumbPath = epub.getThumbBmpPath(COVER_HEIGHT);
      meta.hasCover = Storage.exists(meta.thumbPath.c_str());
      // load uses cached book.bin when present (fast); otherwise parses the EPUB.
      // We don't *force* parsing here — refresh is meant to be cheap. The
      // generateThumbForSlot() path will trigger a full parse when needed.
      if (Storage.exists((epub.getCachePath() + "/book.bin").c_str()) && epub.load(true, true)) {
        meta.title = epub.getTitle();
        meta.author = epub.getAuthor();
      }
      meta.hasBfBadge = BookFusionBookIdStore::hasBookId(path.c_str());
    }
    if (meta.title.empty()) {
      const auto slash = path.find_last_of('/');
      const auto dot = path.find_last_of('.');
      meta.title = path.substr(slash + 1, dot - slash - 1);
    }

    // Progress comes from RecentBooksStore's in-memory list; -1 ("not started")
    // if not present. getBooks() is a vector scan — cheap.
    const auto& recents = RECENT_BOOKS.getBooks();
    auto it = std::find_if(recents.begin(), recents.end(), [&path](const RecentBook& b) { return b.path == path; });
    meta.progressPercent = (it != recents.end()) ? it->progressPercent : -1;
  }
}

int LibraryActivity::findMissingThumbSlot() const {
  const size_t page = currentPage();
  const size_t pageStart = page * pageSize();
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookPaths.size() - pageStart));
  for (int i = 0; i < booksOnPage; ++i) {
    if (!currentPageMeta[i].hasCover) return i;
  }
  return -1;
}

bool LibraryActivity::generateThumbForSlot(int slotIndexInPage) {
  const size_t pageStart = currentPage() * pageSize();
  const std::string path = pathAtLogicalIndex(pageStart + slotIndexInPage);

  // Every exit path sets hasCover=true (even on failure) so that
  // findMissingThumbSlot() doesn't return this slot again, which would loop the
  // Loading popup forever on a genuinely-broken book. drawTileCover handles
  // hasCover=true + missing-file gracefully: it tries to open, fails, falls
  // through to the placeholder. Visually identical to "broken".
  auto& meta = currentPageMeta[slotIndexInPage];

  if (!FsHelpers::hasEpubExtension(path)) {
    meta.hasCover = true;  // .xtc and friends — not generated here; stop polling.
    return false;
  }

  Epub epub(path, "/.crosspoint");
  if (!epub.load(true, true)) {
    meta.hasCover = true;  // Unparseable EPUB; stop polling.
    return true;           // We did slow work; let the chain redraw.
  }

  // Pick up metadata that may have only just been written to book.bin.
  if (meta.title.empty() || meta.title == path.substr(path.find_last_of('/') + 1)) {
    meta.title = epub.getTitle();
  }
  meta.author = epub.getAuthor();

  const std::string thumb = epub.getThumbBmpPath(COVER_HEIGHT);
  meta.thumbPath = thumb;
  if (Storage.exists(thumb.c_str())) {
    meta.hasCover = true;
    return false;  // Already there; no slow work happened.
  }

  epub.generateThumbBmp(COVER_HEIGHT);
  // Unconditional true: a missing file after generate means the EPUB has no
  // cover (or the converter rejected it). Either way, don't retry this slot.
  meta.hasCover = true;
  return true;
}

void LibraryActivity::renderPageFromScratch() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gridTopY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const GridLayout L = computeLayout(screenW, screenH, metrics.contentSidePadding, gridTopY,
                                     pageIndicatorHeight + metrics.buttonHintsHeight, gridRows());

  // Make sure currentPageMeta reflects on-disk state. Cheap — only reads
  // book.bin entries that already exist; never parses an EPUB from scratch.
  refreshCurrentPageMeta();

  const size_t page = currentPage();
  const size_t pageStart = page * pageSize();
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookPaths.size() - pageStart));

  // Draw cover bitmap + border + placeholder for one slot. Matches the cover
  // styling from Lyra3CoversTheme.cpp:46-77.
  auto drawTileCover = [&](int slot) {
    int tileX, tileY;
    tileOrigin(slot, L, tileX, tileY);
    const int coverX = tileX + hPaddingInSelection;
    const int coverY = tileY + hPaddingInSelection;

    bool drewBitmap = false;
    if (currentPageMeta[slot].hasCover && !currentPageMeta[slot].thumbPath.empty()) {
      HalFile bmpFile;
      if (Storage.openFileForRead(MODULE, currentPageMeta[slot].thumbPath.c_str(), bmpFile)) {
        Bitmap bitmap(bmpFile);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          const float bmpW = static_cast<float>(bitmap.getWidth());
          const float bmpH = static_cast<float>(bitmap.getHeight());
          const float ratio = bmpW / bmpH;
          const float tileRatio = static_cast<float>(L.coverDrawW) / static_cast<float>(L.coverDrawH);
          const float cropX = 1.0f - (tileRatio / ratio);
          renderer.drawBitmap(bitmap, coverX, coverY, L.coverDrawW, L.coverDrawH, cropX);
          drewBitmap = true;
        }
        bmpFile.close();
      }
    }

    renderer.drawRect(coverX, coverY, L.coverDrawW, L.coverDrawH, true);
    if (!drewBitmap) {
      // Placeholder verbatim from Lyra3CoversTheme.cpp:69-76.
      renderer.fillRect(coverX, coverY + L.coverDrawH / 3, L.coverDrawW, 2 * L.coverDrawH / 3, true);
      renderer.drawIcon(CoverIcon, coverX + 24, coverY + 24, 32, 32);
    }

    // BookFusion-linked books: bottom-left badge with white padding around the
    // mark, inset from the cover corner. iconY is snapped to a multiple of 8
    // because drawImageTransparent truncates the display-y via integer divide
    // by 8 — non-aligned values shift the icon relative to the white fill.
    if (currentPageMeta[slot].hasBfBadge) {
      constexpr int BF_ICON_SIZE = 24;
      constexpr int BF_PADDING = 4;  // White padding around the icon.
      constexpr int BF_MARGIN = 4;   // Distance from the cover edge.
      constexpr int BF_BADGE_SIZE = BF_ICON_SIZE + 2 * BF_PADDING;
      const int iconY = ((coverY + L.coverDrawH - BF_MARGIN - BF_PADDING - BF_ICON_SIZE) / 8) * 8;
      const int badgeX = coverX + BF_MARGIN;
      const int badgeY = iconY - BF_PADDING;
      const int iconX = badgeX + BF_PADDING;
      renderer.fillRect(badgeX, badgeY, BF_BADGE_SIZE, BF_BADGE_SIZE, false);
      renderer.drawIcon(BookFusion24Icon, iconX, iconY, BF_ICON_SIZE, BF_ICON_SIZE);
    }
  };

  // Fast render — never blocks on cover generation. Missing covers stay as
  // placeholders until loop() fills them in one at a time.
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenW, metrics.headerHeight}, tr(STR_LIBRARY),
                 sortModeLabel(currentSort));
  for (int i = 0; i < booksOnPage; ++i) drawTileCover(i);

  // Snapshot covers+header BEFORE drawing the selection/text layer, so
  // renderSelectionOnly() can restore a clean page and draw a fresh
  // selection band on top. Snapshotting after drawOverlay() would bake
  // in the selection band at slot 0 (initial selectorIndex), causing it
  // to persist behind every subsequent navigation.
  pageBufferStored = storePageBuffer();
  pageRendered = pageBufferStored;

  drawOverlay();

  // === Two-phase load: show placeholders first, generate missing covers second. ===
  // The page is fully drawn at this point with whatever's cached + placeholders
  // for the rest. Push it to the e-ink display NOW so the user sees a populated
  // page immediately, BEFORE we start the slow generation work.
  //
  // Then generate exactly one missing cover and trigger another render. Each
  // subsequent render fills in one more cover until the page is fully cached.
  // Keeping all the slow work on the render task (rather than racing it with
  // loop()) avoids the TaskPriorityDisinherit mutex panic we hit on launch.
  if (contextMenu.isOpen()) return;

  const int missingSlot = findMissingThumbSlot();
  if (missingSlot < 0) return;  // Everything's cached — nothing more to do this render.

  // Draw the Loading popup ON TOP of the placeholder page (not in the snapshot,
  // since we already snapshotted above). The popup is the visual cue that
  // separates "this cover is generating" from "this cover is permanently
  // broken" — once gen is done and the popup disappears, any remaining
  // placeholders are confirmed broken.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  // Push (placeholders + popup) to e-ink so the user sees both immediately.
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
  if (SETTINGS.darkMode) renderer.invertScreen();  // Restore uninverted state.

  // Slow path: generate one missing thumb (~3-5s on first visit per book).
  if (generateThumbForSlot(missingSlot)) {
    // Schedule the next render to redraw with the new cover + start the
    // following thumb. Continues until findMissingThumbSlot returns -1.
    pageBufferStored = false;
    requestUpdate();
  }
}

void LibraryActivity::renderSelectionOnly() {
  if (!restorePageBuffer()) {
    renderPageFromScratch();
    return;
  }
  // The snapshot includes covers but selection/titles are drawn per-frame on
  // top — same idiom as Lyra3CoversTheme (storeCoverBuffer captures covers
  // only; the text + selection layer is redrawn every render call).
  drawOverlay();
}

void LibraryActivity::drawOverlay() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Header sits at metrics.topPadding for headerHeight pixels; grid starts below
  // it after one verticalSpacing of breathing room — matches RecentBooksActivity.cpp:199-202.
  const int gridTopY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const GridLayout L = computeLayout(screenW, screenH, metrics.contentSidePadding, gridTopY,
                                     pageIndicatorHeight + metrics.buttonHintsHeight, gridRows());

  const size_t page = currentPage();
  const size_t pageStart = page * pageSize();
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookPaths.size() - pageStart));
  const int slotInPage = static_cast<int>(selectorIndex - pageStart);

  const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);

  // Number of text rows a slot will draw: up-to-2 title lines + optional author + optional progress.
  auto infoBlockLines = [&](int slot) -> int {
    const int maxLineWidth = L.tileWidth - 2 * hPaddingInSelection;
    auto titleLines = renderer.wrappedText(SMALL_FONT_ID, currentPageMeta[slot].title.c_str(), maxLineWidth, 2);
    int n = static_cast<int>(titleLines.size());
    if (!currentPageMeta[slot].author.empty()) n++;
    if (currentPageMeta[slot].progressPercent >= 0) n++;
    return n;
  };

  // Selection bands first (under text) — matches Lyra3CoversTheme.cpp:99-110.
  if (slotInPage >= 0 && slotInPage < booksOnPage) {
    int tileX, tileY;
    tileOrigin(slotInPage, L, tileX, tileY);
    const int titleBoxH = infoBlockLines(slotInPage) * titleLineHeight + hPaddingInSelection + 5;

    renderer.fillRoundedRect(tileX, tileY, L.tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                             Color::LightGray);
    renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, L.coverDrawH, Color::LightGray);
    renderer.fillRectDither(tileX + L.tileWidth - hPaddingInSelection, tileY + hPaddingInSelection, hPaddingInSelection,
                            L.coverDrawH, Color::LightGray);
    renderer.fillRoundedRect(tileX, tileY + L.coverDrawH + hPaddingInSelection, L.tileWidth, titleBoxH, cornerRadius,
                             false, false, true, true, Color::LightGray);
  }

  // Info rows for every slot: title (up to 2 lines) -> author (truncated) -> progress %.
  for (int i = 0; i < booksOnPage; ++i) {
    int tileX, tileY;
    tileOrigin(i, L, tileX, tileY);
    const int maxLineWidth = L.tileWidth - 2 * hPaddingInSelection;

    auto titleLines = renderer.wrappedText(SMALL_FONT_ID, currentPageMeta[i].title.c_str(), maxLineWidth, 2);
    int textY = tileY + hPaddingInSelection + L.coverDrawH + hPaddingInSelection + 5;
    for (const auto& line : titleLines) {
      renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, textY, line.c_str(), true);
      textY += titleLineHeight;
    }
    if (!currentPageMeta[i].author.empty()) {
      const std::string author = renderer.truncatedText(SMALL_FONT_ID, currentPageMeta[i].author.c_str(), maxLineWidth);
      renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, textY, author.c_str(), true);
      textY += titleLineHeight;
    }
    if (currentPageMeta[i].progressPercent >= 0) {
      char progressBuf[8];
      snprintf(progressBuf, sizeof(progressBuf), "%d%%", currentPageMeta[i].progressPercent);
      renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, textY, progressBuf, true);
    }
  }

  // Page indicator at the bottom — just the page count. The "loading vs broken"
  // distinction is communicated via the centered Loading popup that
  // renderPageFromScratch shows during active generation.
  const int ps = pageSize();
  const size_t pages = (bookPaths.size() + ps - 1) / ps;
  char indicator[40];
  snprintf(indicator, sizeof(indicator), "%zu / %zu", page + 1, pages > 0 ? pages : 1);
  const int indW = renderer.getTextWidth(SMALL_FONT_ID, indicator);
  // Page indicator sits in its own strip just above the button-hints bar.
  const int indStripTop = screenH - metrics.buttonHintsHeight - pageIndicatorHeight;
  const int indY = indStripTop + (pageIndicatorHeight - titleLineHeight) / 2;
  renderer.drawText(SMALL_FONT_ID, (screenW - indW) / 2, indY, indicator, true);

  // Scrollbar on the right edge, alongside the grid. Position + thumb height
  // matches LyraTheme::drawList (LyraTheme.cpp:236-245): vertical track line +
  // filled rect proxy for current page within total pages. Shown only when
  // there's more than one page.
  const int totalP = static_cast<int>(pages);
  const int totalItems = static_cast<int>(bookPaths.size());
  if (totalP > 1 && totalItems > 0) {
    const int barAreaTop = L.topY;
    const int barAreaBottom = screenH - metrics.buttonHintsHeight - pageIndicatorHeight;
    const int barAreaHeight = barAreaBottom - barAreaTop;
    const int barHeight = std::max(8, (barAreaHeight * ps) / totalItems);
    const int barY = barAreaTop + ((barAreaHeight - barHeight) * static_cast<int>(page)) / (totalP - 1);
    const int barX = screenW - metrics.scrollBarRightOffset;
    renderer.drawLine(barX, barAreaTop, barX, barAreaBottom, true);
    renderer.fillRect(barX - metrics.scrollBarWidth, barY, metrics.scrollBarWidth, barHeight, true);
  }
}

void LibraryActivity::dispatchBookAction(BookContextMenu::Action action, const std::string& path,
                                         const std::string& title) {
  // Library-side reload: invalidate snapshot, refresh the sort permutation, restore
  // selector if the list shrank.
  auto reloadAfterMutation = [this] {
    rebuildSortedIndices();
    pageBufferStored = false;
    pageRendered = false;
    lastRenderedPage = static_cast<size_t>(-1);
    if (selectorIndex >= sortedIndices.size() && !sortedIndices.empty()) {
      selectorIndex = sortedIndices.size() - 1;
    }
    requestUpdate();
  };

  auto removeBookFromList = [this](const std::string& p) {
    auto it = std::find(bookPaths.begin(), bookPaths.end(), p);
    if (it != bookPaths.end()) {
      const size_t k = static_cast<size_t>(it - bookPaths.begin());
      bookPaths.erase(it);
      // Keep ALL parallel caches in lockstep so SortEntry views stay valid on
      // rebuild and so the BookFusion badge doesn't shift onto the wrong book.
      if (k < authorCache.size()) authorCache.erase(authorCache.begin() + k);
      if (k < dateAddedCache.size()) dateAddedCache.erase(dateAddedCache.begin() + k);
    }
  };

  switch (action) {
    case BookContextMenu::Action::MarkRead:
      RECENT_BOOKS.updateProgress(path, 100);
      RECENT_BOOKS.saveToFile();
      reloadAfterMutation();
      break;
    case BookContextMenu::Action::ResetProgress:
      RECENT_BOOKS.updateProgress(path, -1);
      RECENT_BOOKS.saveToFile();
      reloadAfterMutation();
      break;
    case BookContextMenu::Action::Shelve:
      // "Remove from recents" — the file stays on disk and remains in the library
      // listing, only the RecentBooksStore entry is cleared.
      RECENT_BOOKS.removeBook(path);
      RECENT_BOOKS.saveToFile();
      reloadAfterMutation();
      break;
    case BookContextMenu::Action::Reindex:
      if (FsHelpers::hasEpubExtension(path)) {
        const std::string sectionsPath = Epub(path, "/.crosspoint").getCachePath() + "/sections";
        Storage.removeDir(sectionsPath.c_str());
      } else if (FsHelpers::hasXtcExtension(path)) {
        Xtc(path, "/.crosspoint").clearCache();
      }
      reloadAfterMutation();
      break;
    case BookContextMenu::Action::Delete:
      startActivityForResult(std::make_unique<ConfirmationActivity>(
                                 renderer, mappedInput, tr(STR_DELETE_FROM_DEVICE) + std::string("?"), title),
                             [this, path, removeBookFromList, reloadAfterMutation](const ActivityResult& res) mutable {
                               if (!res.isCancelled) {
                                 if (FsHelpers::hasEpubExtension(path)) {
                                   Epub(path, "/.crosspoint").clearCache();
                                 }
                                 Storage.remove(path.c_str());
                                 RECENT_BOOKS.removeBook(path);
                                 RECENT_BOOKS.saveToFile();
                                 removeBookFromList(path);
                                 reloadAfterMutation();
                               }
                             });
      break;
    case BookContextMenu::Action::RegenerateCover:
      LOG_DBG(MODULE, "Manual cover regeneration: %s", path.c_str());
      if (FsHelpers::hasEpubExtension(path)) {
        Epub epub(path, "/.crosspoint");
        if (epub.load(false, true)) {
          const std::string thumb = epub.getThumbBmpPath(COVER_HEIGHT);
          if (Storage.exists(thumb.c_str())) Storage.remove(thumb.c_str());
          epub.generateThumbBmp(COVER_HEIGHT);
        }
      } else if (FsHelpers::hasXtcExtension(path)) {
        Xtc xtc(path, "/.crosspoint");
        if (xtc.load()) {
          const std::string thumb = xtc.getThumbBmpPath(COVER_HEIGHT);
          if (Storage.exists(thumb.c_str())) Storage.remove(thumb.c_str());
          xtc.generateThumbBmp(COVER_HEIGHT);
        }
      }
      reloadAfterMutation();
      break;
    case BookContextMenu::Action::BookInfo:
      startActivityForResult(std::make_unique<BookDetailsActivity>(renderer, mappedInput, path, title,
                                                                   contextMenu.author(), contextMenu.progressPercent()),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
  }
}

void LibraryActivity::loop() {
  if (bookPaths.empty()) return;

  const int total = static_cast<int>(bookPaths.size());
  const int pageItems = pageSize();

  // Power short-press → sort menu. Suppressed while the book context menu is open
  // or a sort rebuild is pending.
  if (!contextMenu.isOpen() && !pendingSortRebuild && sortMenu.checkTrigger(mappedInput, currentSort)) {
    pageBufferStored = false;  // Modal overlay invalidates the snapshot.
    requestUpdate();
    return;
  }
  if (sortMenu.isOpen()) {
    SortMode picked;
    bool cancelled = false;
    if (sortMenu.handleInput(buttonNavigator, mappedInput, &picked, &cancelled)) {
      if (!cancelled && picked != currentSort) {
        currentSort = picked;
        SETTINGS.sortMode = static_cast<uint8_t>(picked);
        SETTINGS.saveToFile();
        pendingSortRebuild = true;  // Render will show "Sorting…" and do the work.
        selectorIndex = 0;
        lastRenderedPage = static_cast<size_t>(-1);
        pageBufferStored = false;
      } else {
        pageBufferStored = false;  // Modal overlay invalidates the snapshot.
      }
      requestUpdate();
    } else {
      requestUpdate();
    }
    return;
  }

  // Active modal — drive it and dispatch on terminal events.
  if (contextMenu.isOpen()) {
    BookContextMenu::Action action;
    bool cancelled = false;
    if (contextMenu.handleInput(buttonNavigator, mappedInput, &action, &cancelled)) {
      if (!cancelled) {
        dispatchBookAction(action, contextMenu.path(), contextMenu.title());
      } else {
        pageBufferStored = false;  // Modal overwrote part of the snapshot; force repaint.
        requestUpdate();
      }
    } else {
      requestUpdate();
    }
    return;
  }

  // Long-press Confirm on the selected cover opens the book options modal.
  // Title/author come from currentPageMeta (populated during the page render);
  // progress is looked up in RecentBooksStore — missing means "not started".
  {
    const std::string path = currentPath();
    if (!path.empty()) {
      const int slotInPage = static_cast<int>(selectorIndex - currentPage() * pageSize());
      const std::string& title = currentPageMeta[slotInPage].title;
      const std::string& author = currentPageMeta[slotInPage].author;
      int progress = -1;
      const auto& recents = RECENT_BOOKS.getBooks();
      auto it = std::find_if(recents.begin(), recents.end(), [&path](const RecentBook& b) { return b.path == path; });
      if (it != recents.end()) progress = it->progressPercent;
      if (contextMenu.checkLongPress(mappedInput, path, title, author, progress)) {
        // Tags are short; surface them in the popup when the cached metadata has them.
        // Use the existing cache only (no rebuild) so opening the menu never blocks.
        if (FsHelpers::hasEpubExtension(path)) {
          Epub epub(path, "/.crosspoint");
          if (epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
            contextMenu.setInfoTags(epub.getTags());
          }
        }
        pageBufferStored = false;  // Modal will overwrite the snapshot region.
        requestUpdate();
        return;
      }
    }
  }

  // Short-press Confirm = open selected book.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (contextMenu.consumeLongPressFlag()) return;
    const std::string path = currentPath();
    if (!path.empty()) {
      onSelectBook(path);
      return;
    }
  }

  // Short-press Back = return home.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  // Navigation: short press steps by one, long press (continuous) jumps a page.
  // ButtonNavigator handles wrap-around so going past end -> start and vice versa.
  buttonNavigator.onNextRelease([this, total] {
    const size_t prev = selectorIndex;
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), total);
    // Only force a full repaint if the page changed; otherwise reuse the snapshot.
    if (selectorIndex / pageSize() != prev / pageSize()) pageBufferStored = false;
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, total] {
    const size_t prev = selectorIndex;
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), total);
    if (selectorIndex / pageSize() != prev / pageSize()) pageBufferStored = false;
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, total, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), total, pageItems);
    pageBufferStored = false;  // Page-jump always invalidates the snapshot.
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, total, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), total, pageItems);
    pageBufferStored = false;
    requestUpdate();
  });
}
