#include "LibraryActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

#include "../settings/BookFusionCoverRefreshActivity.h"
#include "../util/ConfirmationActivity.h"
#include "BookDetailsActivity.h"
#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "LibraryScan.h"
#include "ReadingStatsDetailActivity.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/bookfusion24.h"
#include "components/icons/cover.h"
#include "fontIds.h"
#include "util/TouchListNav.h"
#include "util/WifiTimeSync.h"

namespace {
constexpr char MODULE[] = "LIBRARY";
// Tile-internal padding shared with Lyra3CoversTheme.cpp so the selection
// styling and inner cover position match pixel-for-pixel.
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int rowVGap = 16;

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
  frameHoldsPage = false;
  frameOverlayed = false;
  lastDrawnSelector = static_cast<size_t>(-1);
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
  // Index handle, view, hidden set and folder offsets: nothing from the index
  // stays resident once the Library is left (and so nothing while reading).
  closeIndexBacking();
  scanInProgress = false;
  scanCancelRequested = false;
  exitRequested = false;
  bookPaths.clear();
  bookPaths.shrink_to_fit();
  sortedIndices.clear();
  sortedIndices.shrink_to_fit();
  authorCache.clear();
  authorCache.shrink_to_fit();
  tagCache.clear();
  tagCache.shrink_to_fit();
  dateAddedCache.clear();
  dateAddedCache.shrink_to_fit();
  bfBadgeCache.clear();
  bfBadgeCacheReady = false;
  authorCacheReady = false;
  tagCacheReady = false;
  dateAddedCacheReady = false;
  pendingSortRebuild = false;
}

void LibraryActivity::invalidateIndexCache() { LibraryScan::invalidateIndex(); }

void LibraryActivity::enumerateBooks() {
  // Whole-SD enumeration + index caching lives in LibraryScan (shared with the
  // tag browser and the dev-mode metadata recache). Here we only need to reset
  // the bookPaths-indexed metadata caches to match the freshly loaded list.
  LibraryScan::enumerateBooks(bookPaths);

  authorCache.assign(bookPaths.size(), std::string{});
  tagCache.assign(bookPaths.size(), std::string{});
  dateAddedCache.assign(bookPaths.size(), 0u);
  bfBadgeCache.assign(bookPaths.size(), false);
  authorCacheReady = false;
  tagCacheReady = false;
  dateAddedCacheReady = false;
  bfBadgeCacheReady = false;

  rebuildSortedIndices();
}

// --- CLX1 index backing ------------------------------------------------------

namespace {
constexpr uint16_t NO_ORDINAL = 0xFFFF;

// First row of an ascending order whose book has no value for the sort key.
// The index sorts those last, so they form a tail and a binary search finds
// its start in log2(n) record reads.
template <typename IsMissing>
uint16_t knownPrefix(const uint16_t* rows, const uint16_t n, IsMissing&& isMissing) {
  uint16_t lo = 0;
  uint16_t hi = n;
  while (lo < hi) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
    if (isMissing(rows[mid])) {
      hi = mid;
    } else {
      lo = static_cast<uint16_t>(mid + 1);
    }
  }
  return lo;
}

// One book from RecentBooksStore, located in the index by its path hash.
struct RecentHit {
  uint64_t hash;
  uint16_t ordinal;
  int8_t progress;
};
struct RecentLookup {
  RecentHit* hits;
  uint8_t count;
  uint8_t unresolved;
};

bool matchRecent(void* context, const uint16_t ordinal, const uint64_t hash) {
  auto& lookup = *static_cast<RecentLookup*>(context);
  for (uint8_t i = 0; i < lookup.count; i++) {
    if (lookup.hits[i].ordinal == NO_ORDINAL && lookup.hits[i].hash == hash) {
      lookup.hits[i].ordinal = ordinal;
      lookup.unresolved--;
      break;
    }
  }
  return lookup.unresolved > 0;  // stop the scan once every recent book is placed
}

// Last-opened and progress orders. Only the books in RecentBooksStore carry
// either value, so they are promoted (in store order, or by progress) and
// everything else follows in title order, exactly where applySort() puts books
// "missing the criterion". One pass over the index's path hashes; no paths are
// rebuilt and nothing per-book is allocated.
bool orderByReadingHistory(library::LibraryIndexFile& index, const SortMode mode, uint16_t* rows, const uint16_t n) {
  constexpr uint8_t MAX_HITS = 12;  // RecentBooksStore keeps 10
  RecentHit hits[MAX_HITS];         // 192 bytes
  RecentLookup lookup{hits, 0, 0};
  const bool byProgress = mode == SortMode::ProgressMost || mode == SortMode::ProgressLeast;
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (lookup.count == MAX_HITS) break;
    if (byProgress && book.progressPercent < 0) continue;
    hits[lookup.count++] = {library::clixPathHash(book.path.data(), book.path.size()), NO_ORDINAL,
                            book.progressPercent};
  }
  lookup.unresolved = lookup.count;
  if (lookup.count > 0 && !index.forEachPathHash(&matchRecent, &lookup)) return false;

  // Recent books the index does not list (deleted, or added since the scan).
  uint8_t found = 0;
  for (uint8_t i = 0; i < lookup.count; i++) {
    if (hits[i].ordinal != NO_ORDINAL) hits[found++] = hits[i];
  }
  if (mode == SortMode::LastOpenedOldest) {
    std::reverse(hits, hits + found);
  } else if (byProgress) {
    const bool most = mode == SortMode::ProgressMost;
    std::sort(hits, hits + found, [most](const RecentHit& a, const RecentHit& b) {
      if (a.progress != b.progress) return most ? a.progress > b.progress : a.progress < b.progress;
      return a.ordinal < b.ordinal;  // title order breaks ties
    });
  }

  uint16_t out = 0;
  for (uint8_t i = 0; i < found; i++) rows[out++] = hits[i].ordinal;
  for (uint16_t ordinal = 0; ordinal < n && out < n; ordinal++) {
    bool promoted = false;
    for (uint8_t i = 0; i < found; i++) promoted = promoted || hits[i].ordinal == ordinal;
    if (!promoted) rows[out++] = ordinal;
  }
  return out == n;
}
}  // namespace

void LibraryActivity::loadLibrary() {
  closeIndexBacking();
  if (LibraryScan::indexEnabled() && openIndexBacking()) {
    indexMode = true;
    LOG_INF(MODULE, "Library backed by the CLX1 index: %u books", static_cast<unsigned>(viewCount));
    return;
  }
  closeIndexBacking();
  // The user cancelled a scan and there is no earlier index to show: leave the
  // list empty rather than starting the other (uncancellable) card walk.
  // loop() takes the activity Home.
  if (exitRequested.load()) return;
  enumerateBooks();
}

void LibraryActivity::closeIndexBacking() {
  index.close();
  view.reset();
  viewCount = 0;
  hiddenBits.reset();
  folderOffsets.reset();
  folderStride = 0;
  indexMode = false;
  metaPage = static_cast<size_t>(-1);
  pendingViewRebuild = false;
  pendingHideOrdinal = NO_ORDINAL;
  pendingOpen = false;
}

bool LibraryActivity::openIndexBacking() {
  bool cancelled = false;
  if (!LibraryScan::indexIsCurrent()) {
    scanCancelRequested = false;
    scanInProgress = true;
    drawScanScreen(tr(STR_LIBRARY_SCANNING));
    library::BuildCallbacks ui;
    ui.context = this;
    ui.cancelRequested = &LibraryActivity::scanCancelCallback;
    ui.progress = &LibraryActivity::scanProgressCallback;
    const LibraryScan::IndexRefresh result = LibraryScan::refreshIndex(&ui);
    scanInProgress = false;
    cancelled = result == LibraryScan::IndexRefresh::Cancelled;
    if (result != LibraryScan::IndexRefresh::Ok) {
      LOG_ERR(MODULE, "Library scan did not finish (%d); using the previous index if there is one",
              static_cast<int>(result));
    }
  }

  // A failed or cancelled scan leaves the previous index installed. Showing a
  // shelf that is one scan out of date beats showing nothing.
  if (!index.open(LibraryScan::indexPath())) {
    if (cancelled) exitRequested = true;
    return false;
  }

  // 512 bytes for the life of the activity: over the stack budget, and it turns
  // every path read from a walk over all folder records into a short one.
  folderOffsets = makeUniqueNoThrow<uint32_t[]>(library::LibraryIndexFile::FOLDER_CHECKPOINT_COUNT);
  if (!folderOffsets || !index.buildFolderCheckpoints(folderOffsets.get(), folderStride)) {
    folderOffsets.reset();  // paths still resolve, just by walking from the first folder
    folderStride = 0;
  }
  return rebuildView();
}

bool LibraryActivity::indexedPath(const uint16_t ordinal, std::string& out) {
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record)) {
    out.clear();
    return false;
  }
  return folderOffsets ? index.readPath(record, out, folderOffsets.get(), folderStride) : index.readPath(record, out);
}

bool LibraryActivity::rebuildView() {
  const uint16_t n = index.bookCount();
  if (n == 0) {
    view.reset();
    viewCount = 0;
    return true;
  }
  // 2 bytes per book: the footprint sortedIndices has on the in-RAM path, and
  // the only per-book allocation index mode makes. Fallible; the previous view
  // stays in place if this fails. Freed in onExit().
  auto rows = makeUniqueNoThrow<uint16_t[]>(n);
  if (!rows) {
    LOG_ERR(MODULE, "View alloc failed (%u books)", static_cast<unsigned>(n));
    return false;
  }

  using library::SortOrder;
  std::string scratch;
  bool ok = true;
  switch (currentSort) {
    case SortMode::AlphabeticAsc:
    case SortMode::AlphabeticDesc:
    case SortMode::SeriesIndexAsc:  // never offered by this screen; title order
      ok = index.readOrder(SortOrder::TitleAsc, rows.get());
      if (ok && currentSort == SortMode::AlphabeticDesc) std::reverse(rows.get(), rows.get() + n);
      break;
    case SortMode::AuthorAsc:
    case SortMode::AuthorDesc:
      ok = index.readOrder(SortOrder::AuthorAsc, rows.get());
      if (ok && currentSort == SortMode::AuthorDesc) {
        // Books with no author stay last in both directions, as applySort() has it.
        const uint16_t known = knownPrefix(rows.get(), n, [this, &scratch](const uint16_t ordinal) {
          library::ClixRecord record{};
          return !index.readRecord(ordinal, record) || !index.readAuthor(record, scratch);
        });
        std::reverse(rows.get(), rows.get() + known);
      }
      break;
    case SortMode::TagAsc:
    case SortMode::TagDesc:
      ok = index.readOrder(SortOrder::GenreAsc, rows.get());
      if (ok && currentSort == SortMode::TagDesc) {
        const uint16_t known = knownPrefix(rows.get(), n, [this, &scratch](const uint16_t ordinal) {
          library::ClixRecord record{};
          return !index.readRecord(ordinal, record) || !index.readGenre(record, scratch) || scratch.empty();
        });
        std::reverse(rows.get(), rows.get() + known);
      }
      break;
    case SortMode::DateAddedNewest:
    case SortMode::DateAddedOldest:
      // The arrival order runs oldest first.
      ok = index.readOrder(SortOrder::RecentAsc, rows.get());
      if (ok && currentSort == SortMode::DateAddedNewest) std::reverse(rows.get(), rows.get() + n);
      break;
    case SortMode::LastOpenedNewest:
    case SortMode::LastOpenedOldest:
    case SortMode::ProgressMost:
    case SortMode::ProgressLeast:
      ok = orderByReadingHistory(index, currentSort, rows.get(), n);
      break;
    case SortMode::BookFusionFirst:
    case SortMode::BookFusionLast: {
      // The BookFusion link is a sidecar keyed by path, so this is the one order
      // that needs every path (as it does on the in-RAM path). One bit per book,
      // freed at the end of this block.
      auto linked = makeUniqueNoThrow<uint8_t[]>((static_cast<size_t>(n) + 7) / 8);
      ok = static_cast<bool>(linked);
      for (uint16_t ordinal = 0; ok && ordinal < n; ordinal++) {
        ok = indexedPath(ordinal, scratch);
        if (ok && FsHelpers::hasEpubExtension(scratch) && BookFusionBookIdStore::hasBookId(scratch.c_str())) {
          linked[ordinal / 8] |= static_cast<uint8_t>(1u << (ordinal % 8));
        }
        if ((ordinal & 0x0Fu) == 0x0Fu) vTaskDelay(1);  // one SD probe per book: keep the watchdog fed
      }
      if (ok) {
        const bool linkedFirst = currentSort == SortMode::BookFusionFirst;
        uint16_t out = 0;
        for (int pass = 0; pass < 2; pass++) {
          const bool want = (pass == 0) == linkedFirst;
          for (uint16_t ordinal = 0; ordinal < n; ordinal++) {
            const bool isLinked = (linked[ordinal / 8] & (1u << (ordinal % 8))) != 0;
            if (isLinked == want) rows[out++] = ordinal;
          }
        }
      }
      break;
    }
  }
  if (!ok) {
    LOG_ERR(MODULE, "Could not order the Library from the index");
    return false;
  }

  uint16_t count = n;
  if (hiddenBits) {
    count = 0;
    for (uint16_t i = 0; i < n; i++) {
      const uint16_t ordinal = rows[i];
      if ((hiddenBits[ordinal / 8] & (1u << (ordinal % 8))) == 0) rows[count++] = ordinal;
    }
  }
  view = std::move(rows);
  viewCount = count;
  return true;
}

bool LibraryActivity::selectedPath(std::string& out) {
  out.clear();
  if (!indexMode) {
    if (selectorIndex < sortedIndices.size()) out = bookPaths[sortedIndices[selectorIndex]];
    return !out.empty();
  }
  // The page meta and the index handle belong to the render task. A try-lock
  // keeps the input loop from ever waiting behind a slow page render; the
  // caller retries on a later loop() pass.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock() || selectorIndex >= viewCount) return false;
  const size_t page = currentPage();
  if (metaPage == page) {
    out = currentPageMeta[selectorIndex - page * pageSize()].path;
  } else {
    indexedPath(view[selectorIndex], out);
  }
  return !out.empty();
}

void LibraryActivity::drawScanScreen(const char* message) {
  // A full repaint every time: EPUB metadata reads borrow the framebuffer as
  // inflate scratch on the C3 boards, so nothing drawn before survives.
  renderer.clearScreen();
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // drawPopup() applies dark mode and pushes the whole buffer, hints included.
  GUI.drawPopup(renderer, message);
}

bool LibraryActivity::scanCancelCallback(void* context) {
  return static_cast<const LibraryActivity*>(context)->scanCancelRequested.load();
}

void LibraryActivity::scanProgressCallback(void* context, const library::BuildProgress& progress) {
  auto* self = static_cast<LibraryActivity*>(context);
  if (progress.phase == library::BuildPhase::Organizing) {
    self->drawScanScreen(tr(STR_LIBRARY_ORGANIZING));
    return;
  }
  char message[64];
  snprintf(message, sizeof(message), tr(STR_LIBRARY_SCAN_COUNT), static_cast<unsigned>(progress.books));
  self->drawScanScreen(message);
}

bool LibraryActivity::siblingPathCallback(void* context, const int pos, std::string& out) {
  // Called by the Book Details child, while this activity sits under it on the
  // stack and is not rendering: nothing else is using the index handle.
  auto* self = static_cast<LibraryActivity*>(context);
  if (!self->indexMode || pos < 0 || pos >= static_cast<int>(self->viewCount)) return false;
  return self->indexedPath(self->view[pos], out);
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
  if (indexMode) {
    // Every order comes from the index: no per-book metadata pass, no caches.
    rebuildView();
    return;
  }
  const bool needsAuthor =
      (currentSort == SortMode::AuthorAsc || currentSort == SortMode::AuthorDesc) && !authorCacheReady;
  const bool needsTag = (currentSort == SortMode::TagAsc || currentSort == SortMode::TagDesc) && !tagCacheReady;
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

  if (needsTag) {
    // Only EPUBs carry dc:subject tags; XTC has no tag metadata, so those rows
    // stay empty and sink to the end of the Tag sorts.
    for (size_t i = 0; i < bookPaths.size(); i++) {
      const std::string& path = bookPaths[i];
      if (FsHelpers::hasEpubExtension(path)) {
        Epub epub(path, "/.crosspoint");
        if (Storage.exists((epub.getCachePath() + "/book.bin").c_str()) && epub.load(true, true)) {
          tagCache[i] = epub.getTags();
        }
      }
    }
    tagCacheReady = true;
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
      bfBadgeCache[i] =
          FsHelpers::hasEpubExtension(bookPaths[i]) && BookFusionBookIdStore::hasBookId(bookPaths[i].c_str());
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
    e.tagKey = tagCacheReady ? std::string_view(tagCache[i]) : std::string_view{};
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

std::string LibraryActivity::pathAtLogicalIndex(size_t logicalIdx) {
  if (indexMode) {
    std::string path;
    if (logicalIdx < viewCount) indexedPath(view[logicalIdx], path);
    return path;
  }
  if (logicalIdx >= sortedIndices.size()) return {};
  return bookPaths[sortedIndices[logicalIdx]];
}

LibraryActivity::SlotRect LibraryActivity::slotRect(int slotIndexInPage) const {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Header sits at metrics.topPadding for headerHeight pixels; grid starts below
  // it after one verticalSpacing of breathing room — matches RecentBooksActivity.cpp:199-202.
  const int gridTopY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const GridLayout L = computeLayout(screenW, screenH, metrics.contentSidePadding, gridTopY,
                                     metrics.pageIndicatorHeight + metrics.buttonHintsHeight, gridRows());

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
  const size_t bufferSize = renderer.getBufferSize();
  // Reuse the snapshot across page flips and cover fills: re-mallocing 48KB
  // per page draw churns the largest free block for nothing.
  if (!pageBuffer) {
    pageBuffer = static_cast<uint8_t*>(malloc(bufferSize));
    if (!pageBuffer) {
      LOG_ERR(MODULE, "Failed to malloc %zu byte page buffer", bufferSize);
      return false;
    }
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
  frameHoldsPage = false;
}

void LibraryActivity::render(RenderLock&&) {
  // Initial entry: paint a "Loading…" popup over whatever the prior activity
  // left in the framebuffer, push it to e-ink, THEN run the (synchronous) SD
  // BFS. Without this, the device looks frozen for a second or two on a
  // populated library. Falls through to the normal render path so the
  // populated page paints in the same render call.
  if (initialLoadPending) {
    GUI.drawPopup(renderer, tr(STR_LOADING));
    // drawPopup() has already pushed this frame. In light mode a second
    // displayBuffer() would repaint the identical buffer, so only dark mode
    // (where the polarity differs from what drawPopup showed) refreshes again.
    if (SETTINGS.darkMode) {
      renderer.invertScreen();
      renderer.displayBuffer();
      renderer.invertScreen();
    }

    // Preempt the background boot-time NTP sync (if still running) so its WiFi
    // stack heap is freed before the metadata recache's heavy allocations —
    // avoids a cold-boot OOM/contention crash. No-op after the ~10s boot window.
    WifiTimeSync::preempt();
    {
      const uint32_t t0 = millis();
      loadLibrary();
      LOG_INF("LIBTIME", "load+sort %u books (%s): %lu ms", static_cast<unsigned>(bookCount()),
              indexMode ? "index" : "path list", static_cast<unsigned long>(millis() - t0));
    }
    initialLoadPending = false;
    // A scan repaints the panel and lends the framebuffer out as scratch.
    frameHoldsPage = false;
    pageBufferStored = false;
    lastRenderedPage = static_cast<size_t>(-1);
    // Fall through.
  }

  // Index mode: list changes requested from loop() or an activity result (a
  // deleted book, a progress change that moves a book in the order) are applied
  // here, on the task that owns the index handle and the view.
  if (indexMode && pendingViewRebuild) {
    pendingViewRebuild = false;
    if (pendingHideOrdinal != 0xFFFF && pendingHideOrdinal < index.bookCount()) {
      // One bit per book, allocated on the first deletion of this visit.
      if (!hiddenBits) hiddenBits = makeUniqueNoThrow<uint8_t[]>((static_cast<size_t>(index.bookCount()) + 7) / 8);
      if (hiddenBits) {
        hiddenBits[pendingHideOrdinal / 8] |= static_cast<uint8_t>(1u << (pendingHideOrdinal % 8));
      } else {
        LOG_ERR(MODULE, "Hidden-set alloc failed; the deleted book stays listed until the next scan");
      }
    }
    pendingHideOrdinal = 0xFFFF;
    rebuildView();
    if (selectorIndex >= viewCount) selectorIndex = viewCount == 0 ? 0 : viewCount - 1;
    lastRenderedPage = static_cast<size_t>(-1);
    pageBufferStored = false;
    frameHoldsPage = false;
  }

  // A sort mode change can require a metadata pass (book.bin parsing, SD stats)
  // that takes seconds for large libraries. Show a "Sorting…" popup *before*
  // running the rebuild so the user sees feedback rather than a frozen modal.
  if (pendingSortRebuild) {
    GUI.drawPopup(renderer, tr(STR_SORTING));
    // drawPopup() has already pushed this frame. In light mode a second
    // displayBuffer() would repaint the identical buffer, so only dark mode
    // (where the polarity differs from what drawPopup showed) refreshes again.
    if (SETTINGS.darkMode) {
      renderer.invertScreen();
      renderer.displayBuffer();
      renderer.invertScreen();
    }

    {
      const uint32_t t0 = millis();
      rebuildSortedIndices();
      LOG_INF("LIBTIME", "sort rebuild: %lu ms", static_cast<unsigned long>(millis() - t0));
    }
    pendingSortRebuild = false;
    lastRenderedPage = static_cast<size_t>(-1);
    pageBufferStored = false;
    // Fall through to the normal render path so the new sorted page is drawn.
  }

  if (bookCount() == 0) {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   tr(STR_LIBRARY), sortModeLabel(currentSort), contextMenu.isOpen() ? nullptr : tr(STR_SORT));
    drawButtonHints();
    contextMenu.render(renderer);
    sortMenu.render(renderer);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  const size_t page = currentPage();
  const bool pageChanged = (page != lastRenderedPage);
  const bool menuOpen = contextMenu.isOpen() || sortMenu.isOpen();

  const uint32_t tPage = millis();
  const char* pathName;
  if (pageChanged || (!pageBufferStored && (!frameHoldsPage || (frameOverlayed && !menuOpen)))) {
    // New page, or the frame no longer holds a clean page and no snapshot can restore one.
    renderPageFromScratch();
    lastRenderedPage = page;
    pathName = "scratch";
  } else if (pageBufferStored) {
    renderSelectionOnly();
    pathName = "snapshot";
  } else if (menuOpen) {
    // Both menus paint an opaque box at a fixed spot and redraw over
    // themselves below; the page underneath is untouched.
    pathName = "menu";
  } else {
    // No snapshot, but the frame still holds this page's covers: the band
    // never overlaps a cover, so only the tile that had it and the one that
    // gets it need their chrome cleared before the text layer is redrawn.
    const size_t pageStart = page * pageSize();
    if (lastDrawnSelector / pageSize() == page) clearTileChrome(static_cast<int>(lastDrawnSelector - pageStart));
    clearTileChrome(static_cast<int>(selectorIndex - pageStart));
    drawOverlay();
    pathName = "chrome";
  }
  const uint32_t tPageDone = millis();

  drawButtonHints();
  contextMenu.render(renderer);
  sortMenu.render(renderer);

  // Two-phase load: the page just drawn shows cached covers + placeholders.
  // Push it (with a Loading popup while covers are still missing) FIRST, then
  // generate one cover. The popup separates "generating" from "permanently
  // broken" — once it's gone, any remaining placeholder is confirmed broken.
  // Keeping the slow work on the render task (rather than racing loop())
  // avoids the TaskPriorityDisinherit mutex panic we hit on launch.
  const int missingSlot = (contextMenu.isOpen() || sortMenu.isOpen()) ? -1 : findMissingThumbSlot();
  if (missingSlot >= 0) GUI.drawPopup(renderer, tr(STR_LOADING));

  if (SETTINGS.darkMode) renderer.invertScreen();
  const uint32_t tDisp = millis();
  renderer.displayBuffer();
  const uint32_t tDispDone = millis();
  // Undo the dark-mode inversion so the frame stays in logical form for the
  // snapshot-less chrome repaint and the menu redraw above.
  if (SETTINGS.darkMode) renderer.invertScreen();
  frameOverlayed = menuOpen || missingSlot >= 0;
  LOG_INF("LIBTIME", "render page=%u %s: draw %lu ms, hints+menus %lu ms, display %lu ms, missingSlot=%d",
          static_cast<unsigned>(page), pathName, static_cast<unsigned long>(tPageDone - tPage),
          static_cast<unsigned long>(tDisp - tPageDone), static_cast<unsigned long>(tDispDone - tDisp), missingSlot);

  if (missingSlot >= 0) {
    const uint32_t t0 = millis();
    fillMissingCover(missingSlot);
    LOG_INF("LIBTIME", "fillMissingCover slot=%d: %lu ms", missingSlot, static_cast<unsigned long>(millis() - t0));
  }
}

void LibraryActivity::fillMissingCover(const int slotIndexInPage) {
  const size_t page = currentPage();
  // Slow path: ~1-5s on the first visit per book.
  generateThumbForSlot(slotIndexInPage);
  // Paint only the new tile over the snapshot instead of redrawing the page:
  // the other covers are already in it, and a from-scratch render re-opens
  // every thumb and book.bin on the page. The next render restores the
  // snapshot, redraws the overlay (picking up the fresh title/author), and
  // starts the following cover. Always reschedule, even when nothing slow ran
  // (e.g. a BookFusion book with no cached cover): coverGenAttempted stops the
  // chain, and the final render drops the popup.
  if (pageBufferStored && page == currentPage() && page == lastRenderedPage && restorePageBuffer()) {
    drawTileCover(slotIndexInPage);
    pageBufferStored = storePageBuffer();
  } else {
    pageBufferStored = false;
  }
  requestUpdate();
}

void LibraryActivity::drawButtonHints() {
  // Same label set as HomeActivity (HomeActivity.cpp:407-409). When the book
  // options modal is open, btn1 shows Back to cancel; otherwise it's empty
  // because short-press Back goes home (long-press isn't yet bound to a label).
  // Sort menu open: Back closes, Confirm selects the field / flips its direction.
  const auto labels = (contextMenu.isOpen() || sortMenu.isOpen())
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Power short-press opens the sort menu; surface that as a sideways hint. Hidden while a
  // modal is open (the context menu suppresses Power-to-sort; the sort menu is already up).
  if (!contextMenu.isOpen() && !sortMenu.isOpen()) GUI.drawPowerButtonHint(renderer, tr(STR_SORT));
}

void LibraryActivity::refreshCurrentPageMeta() {
  const size_t page = currentPage();
  const size_t pageStart = page * pageSize();
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookCount() - pageStart));

  // loop() must not read the slots while they are being rewritten; it checks
  // this (under the render lock) before trusting them.
  metaPage = static_cast<size_t>(-1);

  // Reset all slots so stale data from a previous page doesn't leak.
  for (auto& m : currentPageMeta) {
    m.title.clear();
    m.author.clear();
    m.progressPercent = -1;
    m.hasCover = false;
    m.hasBfBadge = false;
    m.coverResolved = false;
    m.thumbPath.clear();
    m.path.clear();
    m.ordinal = 0xFFFF;
  }

  for (int i = 0; i < booksOnPage; ++i) {
    const uint32_t t0 = millis();
    auto& meta = currentPageMeta[i];
    std::string path;
    if (indexMode) {
      // Title, author and path come out of the one open index file by seeks:
      // no book.bin open, so no /.crosspoint path resolve for the text at all.
      meta.ordinal = view[pageStart + i];
      library::ClixRecord record{};
      if (index.readRecord(meta.ordinal, record)) {
        index.readDisplayText(record, meta.title, meta.author);
        if (folderOffsets) {
          index.readPath(record, path, folderOffsets.get(), folderStride);
        } else {
          index.readPath(record, path);
        }
      }
      meta.path = path;
    } else {
      path = pathAtLogicalIndex(pageStart + i);
    }

    if (FsHelpers::hasEpubExtension(path)) {
      Epub epub(path, "/.crosspoint");
      // Every path probe here is a FAT long-name resolve through /.crosspoint
      // (~20-25 ms each on the X3 with a populated library), so the cover is
      // not probed at all: drawTileCover opens the thumb, falls back to the
      // BookFusion API cover.bmp (see there) and records the outcome in
      // hasCover/coverResolved.
      meta.thumbPath = epub.getThumbBmpPath(COVER_HEIGHT);
      meta.hasCover = true;
      meta.coverResolved = false;
      // Same reason: no exists() probe before the load. With buildIfMissing
      // false a missing book.bin fails on its single open instead of parsing
      // the EPUB; generateThumbForSlot() is the path that builds it.
      if (!indexMode && epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
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
    LOG_DBG("LIBTIME", "  meta %d: %lu ms (cover=%d)", i, static_cast<unsigned long>(millis() - t0),
            meta.hasCover ? 1 : 0);
  }
  metaPage = page;
}

int LibraryActivity::findMissingThumbSlot() const {
  const size_t page = currentPage();
  const size_t pageStart = page * pageSize();
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookCount() - pageStart));
  for (int i = 0; i < booksOnPage; ++i) {
    // Skip slots we've already tried this page visit: a cover that fails to
    // generate leaves hasCover=false on disk, so without this guard the loader
    // would re-select the same slot every render and never reach the rest.
    if (!currentPageMeta[i].hasCover && !coverGenAttempted[i]) return i;
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

  // Record the attempt up front so findMissingThumbSlot won't re-select this slot
  // on the forced re-render below, even if generation fails to produce a file.
  coverGenAttempted[slotIndexInPage] = true;

  if (!FsHelpers::hasEpubExtension(path)) {
    meta.hasCover = true;  // .xtc and friends — not generated here; stop polling.
    return false;
  }

  // TEMP diagnostic (crash triage): heap state before a per-book cover/metadata build.
  // Remove once the browse crash is root-caused.
  LOG_DBG("MEMDIAG", "thumb slot=%d free=%u largest=%u", slotIndexInPage, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  const uint32_t tLoad = millis();
  Epub epub(path, "/.crosspoint");
  if (!epub.load(true, true)) {
    meta.hasCover = true;  // Unparseable EPUB; stop polling.
    return true;           // We did slow work; let the chain redraw.
  }
  LOG_INF("LIBTIME", "thumb slot=%d epub.load: %lu ms", slotIndexInPage, static_cast<unsigned long>(millis() - tLoad));

  // Pick up metadata that may have only just been written to book.bin.
  if (meta.title.empty() || meta.title == path.substr(path.find_last_of('/') + 1)) {
    meta.title = epub.getTitle();
  }
  // Index mode already shows the index's (spelling-harmonised) author.
  if (!indexMode) meta.author = epub.getAuthor();

  const std::string thumb = epub.getThumbBmpPath(COVER_HEIGHT);
  meta.thumbPath = thumb;
  if (Storage.exists(thumb.c_str())) {
    meta.hasCover = true;
    return false;  // Already there; no slow work happened.
  }

  // BookFusion books: NEVER derive a cover from the EPUB. BookFusion-served EPUBs
  // frequently carry broken/unreliable cover images; the only trustworthy cover is
  // the already-normalised image from the API, cached at download as thumb_<H>.bmp
  // and cover.bmp (both checked in refreshCurrentPageMeta). If neither is present
  // the API cover failed to cache — show the placeholder rather than extracting a
  // bad cover from the EPUB.
  if (BookFusionBookIdStore::hasBookId(path.c_str())) {
    meta.hasCover = true;  // stop polling; placeholder until the API cover is re-cached
    return false;
  }

  const uint32_t tGen = millis();
  epub.generateThumbBmp(COVER_HEIGHT);
  LOG_INF("LIBTIME", "thumb slot=%d generateThumbBmp: %lu ms", slotIndexInPage,
          static_cast<unsigned long>(millis() - tGen));
  // Unconditional true: a missing file after generate means the EPUB has no
  // cover (or the converter rejected it). Either way, don't retry this slot.
  meta.hasCover = true;
  return true;
}

void LibraryActivity::renderPageFromScratch() {
  const int screenW = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Make sure currentPageMeta reflects on-disk state. Cheap — only reads
  // book.bin entries that already exist; never parses an EPUB from scratch.
  const uint32_t tMeta = millis();
  refreshCurrentPageMeta();
  const uint32_t tMetaDone = millis();

  const size_t page = currentPage();
  // A fresh page (navigation, sort, reload — all of which reset lastRenderedPage
  // or move it off this page) clears the cover-generation attempt log so every
  // slot gets one try. A repaint of the same page (a modal invalidated the
  // snapshot mid cover-fill) keeps it, so a slot whose cover can't be generated
  // is skipped instead of retried forever.
  if (page != lastRenderedPage) coverGenAttempted.fill(false);

  const size_t pageStart = page * pageSize();
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookCount() - pageStart));

  // Fast render — never blocks on cover generation. Missing covers stay as
  // placeholders; render() fills them in one at a time after pushing this page.
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenW, metrics.headerHeight}, tr(STR_LIBRARY),
                 sortModeLabel(currentSort), contextMenu.isOpen() ? nullptr : tr(STR_SORT));
  const uint32_t tTiles = millis();
  for (int i = 0; i < booksOnPage; ++i) {
    const uint32_t t0 = millis();
    drawTileCover(i);
    LOG_DBG("LIBTIME", "  tile %d (%s): %lu ms", i, currentPageMeta[i].hasCover ? "bmp" : "placeholder",
            static_cast<unsigned long>(millis() - t0));
  }
  const uint32_t tTilesDone = millis();

  // Snapshot covers+header BEFORE drawing the selection/text layer, so
  // renderSelectionOnly() can restore a clean page and draw a fresh
  // selection band on top. Snapshotting after drawOverlay() would bake
  // in the selection band at slot 0 (initial selectorIndex), causing it
  // to persist behind every subsequent navigation.
  pageBufferStored = storePageBuffer();
  frameHoldsPage = true;

  const uint32_t tOverlay = millis();
  drawOverlay();
  LOG_INF("LIBTIME", "scratch: meta %lu ms, %d tiles %lu ms, snapshot %lu ms, overlay %lu ms",
          static_cast<unsigned long>(tMetaDone - tMeta), booksOnPage, static_cast<unsigned long>(tTilesDone - tTiles),
          static_cast<unsigned long>(tOverlay - tTilesDone), static_cast<unsigned long>(millis() - tOverlay));
}

// Draw cover bitmap + border + placeholder for one slot. Matches the cover
// styling from Lyra3CoversTheme.cpp:46-77.
void LibraryActivity::drawTileCover(const int slot) {
  const SlotRect r = slotRect(slot);
  const int coverX = r.x;
  const int coverY = r.y;
  // fillMissingCover paints over a placeholder already in the snapshot.
  renderer.fillRect(coverX, coverY, r.width, r.height, false);

  bool drewBitmap = false;
  auto& meta = currentPageMeta[slot];
  if (meta.hasCover && !meta.thumbPath.empty()) {
    // Storage.open rather than openFileForRead: a missing thumb is an expected
    // miss here, not an error line per render.
    HalFile bmpFile = Storage.open(meta.thumbPath.c_str());
    if (!bmpFile && !meta.coverResolved) {
      // BookFusion EPUBs frequently ship with no embedded cover image — the cover
      // lives only on BookFusion's servers and is fetched at download time, cached
      // as both thumb_<H>.bmp and the full sleep-screen cover.bmp. If the tile
      // thumbnail is missing, render that full cover instead of falling back to
      // (futile) EPUB extraction, which has nothing to extract and only yields a
      // placeholder. The scale/crop below fits any BMP to the tile. Path mirrors
      // Epub::getCoverBmpPath(false) without constructing an Epub.
      const std::string bfCover = meta.thumbPath.substr(0, meta.thumbPath.find_last_of('/')) + "/cover.bmp";
      bmpFile = Storage.open(bfCover.c_str());
      if (bmpFile) meta.thumbPath = bfCover;
    }
    meta.coverResolved = true;
    if (!bmpFile) {
      meta.hasCover = false;  // Neither file exists: placeholder; findMissingThumbSlot may generate one.
    } else {
      Bitmap bitmap(bmpFile);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const float bmpW = static_cast<float>(bitmap.getWidth());
        const float bmpH = static_cast<float>(bitmap.getHeight());
        const float ratio = bmpW / bmpH;
        const float tileRatio = static_cast<float>(r.width) / static_cast<float>(r.height);
        const float cropX = 1.0f - (tileRatio / ratio);
        renderer.drawBitmap(bitmap, coverX, coverY, r.width, r.height, cropX);
        drewBitmap = true;
      }
      bmpFile.close();
    }
  }

  renderer.drawRect(coverX, coverY, r.width, r.height, true);
  if (!drewBitmap) {
    // Placeholder verbatim from Lyra3CoversTheme.cpp:69-76.
    renderer.fillRect(coverX, coverY + r.height / 3, r.width, 2 * r.height / 3, true);
    renderer.drawIcon(CoverIcon, coverX + 24, coverY + 24, 32, 32);
  }

  // BookFusion-linked books: bottom-left badge with white padding around the
  // mark, inset from the cover corner. iconY is snapped to a multiple of 8
  // because drawImageTransparent truncates the display-y via integer divide
  // by 8 — non-aligned values shift the icon relative to the white fill.
  if (meta.hasBfBadge) {
    constexpr int BF_ICON_SIZE = 24;
    constexpr int BF_PADDING = 4;  // White padding around the icon.
    constexpr int BF_MARGIN = 4;   // Distance from the cover edge.
    constexpr int BF_BADGE_SIZE = BF_ICON_SIZE + 2 * BF_PADDING;
    const int iconY = ((coverY + r.height - BF_MARGIN - BF_PADDING - BF_ICON_SIZE) / 8) * 8;
    const int badgeX = coverX + BF_MARGIN;
    const int badgeY = iconY - BF_PADDING;
    const int iconX = badgeX + BF_PADDING;
    renderer.fillRect(badgeX, badgeY, BF_BADGE_SIZE, BF_BADGE_SIZE, false);
    renderer.drawIcon(BookFusion24Icon, iconX, iconY, BF_ICON_SIZE, BF_ICON_SIZE);
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

void LibraryActivity::clearTileChrome(const int slot) {
  if (slot < 0 || slot >= pageSize()) return;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gridTopY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const GridLayout L = computeLayout(screenW, screenH, metrics.contentSidePadding, gridTopY,
                                     metrics.pageIndicatorHeight + metrics.buttonHintsHeight, gridRows());
  int tileX, tileY;
  tileOrigin(slot, L, tileX, tileY);
  const int coverTop = tileY + hPaddingInSelection;
  const int coverBottom = coverTop + L.coverDrawH;
  // Top strip, both side strips, then the title box and text rows down to the
  // end of the row (the row height is what drawOverlay's text is sized to).
  renderer.fillRect(tileX, tileY, L.tileWidth, hPaddingInSelection, false);
  renderer.fillRect(tileX, coverTop, hPaddingInSelection, L.coverDrawH, false);
  renderer.fillRect(tileX + L.tileWidth - hPaddingInSelection, coverTop, hPaddingInSelection, L.coverDrawH, false);
  renderer.fillRect(tileX, coverBottom, L.tileWidth, L.rowH - hPaddingInSelection - L.coverDrawH, false);
}

void LibraryActivity::drawOverlay() {
  lastDrawnSelector = selectorIndex;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Header sits at metrics.topPadding for headerHeight pixels; grid starts below
  // it after one verticalSpacing of breathing room — matches RecentBooksActivity.cpp:199-202.
  const int gridTopY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const GridLayout L = computeLayout(screenW, screenH, metrics.contentSidePadding, gridTopY,
                                     metrics.pageIndicatorHeight + metrics.buttonHintsHeight, gridRows());

  const size_t page = currentPage();
  const size_t pageStart = page * pageSize();
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookCount() - pageStart));
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

  // Page count, in the strip every scrollable screen reserves for it: the grid
  // pages the same way a list does, so it says so the same way. The "loading vs
  // broken" distinction is communicated via the centered Loading popup that
  // renderPageFromScratch shows during active generation.
  const int ps = pageSize();
  const size_t pages = (bookCount() + ps - 1) / ps;
  // The rect the grid was laid out in, ending where the hint bar starts —
  // drawPageIndicator takes its strip out of the bottom of that.
  const Rect gridRect{0, L.topY, screenW, screenH - metrics.buttonHintsHeight - L.topY};
  GUI.drawPageIndicator(renderer, gridRect, static_cast<int>(page) + 1, static_cast<int>(pages));

  // Scrollbar on the right edge, alongside the grid. Position + thumb height
  // matches LyraTheme::drawList (LyraTheme.cpp:236-245): vertical track line +
  // filled rect proxy for current page within total pages. Shown only when
  // there's more than one page.
  const int totalP = static_cast<int>(pages);
  const int totalItems = static_cast<int>(bookCount());
  if (totalP > 1 && totalItems > 0) {
    const int barAreaTop = L.topY;
    const int barAreaBottom = screenH - metrics.buttonHintsHeight - metrics.pageIndicatorHeight;
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
    if (indexMode) {
      // The view and the index handle belong to the render task: ask it to
      // rebuild (it also re-clamps the selector) instead of doing it here.
      pendingViewRebuild = true;
      pageBufferStored = false;
      frameHoldsPage = false;
      lastRenderedPage = static_cast<size_t>(-1);
      requestUpdate();
      return;
    }
    rebuildSortedIndices();
    pageBufferStored = false;
    frameHoldsPage = false;
    lastRenderedPage = static_cast<size_t>(-1);
    if (selectorIndex >= sortedIndices.size() && !sortedIndices.empty()) {
      selectorIndex = sortedIndices.size() - 1;
    }
    requestUpdate();
  };

  auto removeBookFromList = [this](const std::string& p) {
    if (indexMode) {
      // The index on the card still lists the book until the next scan, so it
      // is hidden by ordinal for the rest of this visit. Activity result
      // handlers run without the render lock; the page meta is the render
      // task's, so take the lock for the lookup.
      RenderLock lock;
      pendingHideOrdinal = 0xFFFF;
      for (const auto& m : currentPageMeta) {
        if (m.ordinal != 0xFFFF && m.path == p) {
          pendingHideOrdinal = m.ordinal;
          break;
        }
      }
      // Not on the page the meta describes (should not happen): reload, which
      // rescans because the deletion below invalidated the index.
      if (pendingHideOrdinal == 0xFFFF) initialLoadPending = true;
      return;
    }
    auto it = std::find(bookPaths.begin(), bookPaths.end(), p);
    if (it != bookPaths.end()) {
      const size_t k = static_cast<size_t>(it - bookPaths.begin());
      bookPaths.erase(it);
      // Keep ALL parallel caches in lockstep so SortEntry views stay valid on
      // rebuild and so the BookFusion badge doesn't shift onto the wrong book.
      if (k < authorCache.size()) authorCache.erase(authorCache.begin() + k);
      if (k < tagCache.size()) tagCache.erase(tagCache.begin() + k);
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
    case BookContextMenu::Action::Pin:
      RECENT_BOOKS.togglePin(path);
      reloadAfterMutation();
      break;
    case BookContextMenu::Action::Shelve:
      // "Remove from recents" — the file stays on disk and remains in the library
      // listing, only the RecentBooksStore entry is cleared (and the Bookshelf
      // home stops offering it as a filler).
      RECENT_BOOKS.shelveBook(path);
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
                                 // The persisted indexes still list the file (SdFat does not
                                 // touch the folder's mtime on remove), as GroupBrowser's
                                 // delete already accounts for.
                                 LibraryScan::invalidateIndex();
                                 removeBookFromList(path);
                                 reloadAfterMutation();
                               }
                             });
      break;
    case BookContextMenu::Action::RegenerateCover:
      LOG_DBG(MODULE, "Manual cover regeneration: %s", path.c_str());
      if (FsHelpers::hasEpubExtension(path) && BookFusionBookIdStore::hasBookId(path.c_str())) {
        // BookFusion EPUB covers are unreliable; re-download the API cover instead.
        startActivityForResult(std::make_unique<BookFusionCoverRefreshActivity>(renderer, mappedInput, path, title),
                               [reloadAfterMutation](const ActivityResult&) { reloadAfterMutation(); });
        break;
      }
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
    case BookContextMenu::Action::BookInfo: {
      if (indexMode) {
        // Same Left/Right paging through the whole library, but there is no
        // path vector to lend: the child asks for one path at a time, read from
        // the index this (still stacked) activity keeps open.
        auto details = std::make_unique<BookDetailsActivity>(renderer, mappedInput, path, title, contextMenu.author(),
                                                             contextMenu.progressPercent());
        details->setSiblingsResolver(this, &LibraryActivity::siblingPathCallback, static_cast<int>(viewCount),
                                     static_cast<int>(selectorIndex));
        startActivityForResult(std::move(details), [this](const ActivityResult&) { requestUpdate(); });
        break;
      }
      // Let BookDetails page Left/Right through the whole library in display order.
      // bookPaths outlives the child (parent stays on the activity stack); only the
      // uint16 order (a copy of sortedIndices) is owned by the child.
      std::vector<uint16_t> order = sortedIndices;
      int pos = static_cast<int>(selectorIndex);
      if (pos < 0 || pos >= static_cast<int>(order.size()) || order[pos] >= bookPaths.size() ||
          bookPaths[order[pos]] != path) {
        pos = 0;
        for (size_t i = 0; i < order.size(); ++i) {
          if (order[i] < bookPaths.size() && bookPaths[order[i]] == path) {
            pos = static_cast<int>(i);
            break;
          }
        }
      }
      auto details = std::make_unique<BookDetailsActivity>(renderer, mappedInput, path, title, contextMenu.author(),
                                                           contextMenu.progressPercent());
      details->setSiblingsRef(&bookPaths, std::move(order), pos);
      startActivityForResult(std::move(details), [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case BookContextMenu::Action::ViewStats:
      startActivityForResult(std::make_unique<ReadingStatsDetailActivity>(renderer, mappedInput, path),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
  }
}

int LibraryActivity::slotIndexAt(const int lx, const int ly) const {
  const int total = static_cast<int>(bookCount());
  const int pageStart = static_cast<int>(currentPage()) * pageSize();
  for (int slot = 0; slot < pageSize(); slot++) {
    if (pageStart + slot >= total) break;  // empty trailing slots miss
    const SlotRect r = slotRect(slot);
    if (lx >= r.x && lx < r.x + r.width && ly >= r.y && ly < r.y + r.height) {
      return slot;
    }
  }
  return -1;
}

void LibraryActivity::loop() {
  // A Library scan runs on the render task; this loop keeps polling input on
  // the main task, so Back (or its tappable hint on the X4 Pro) cancels it. The
  // builder polls the flag every 50 ms and leaves the previous index in place.
  if (scanInProgress.load()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) scanCancelRequested = true;
    return;
  }
  if (exitRequested.exchange(false)) {
    onGoHome();
    return;
  }
  if (initialLoadPending || bookCount() == 0) return;

  const int total = static_cast<int>(bookCount());
  const int pageItems = pageSize();

  // Power short-press → sort menu. Suppressed while the book context menu is open
  // or a sort rebuild is pending.
  if (!contextMenu.isOpen() && !pendingSortRebuild &&
      sortMenu.checkTrigger(mappedInput, currentSort, ALL_SORT_OPTIONS, ALL_SORT_OPTIONS_COUNT)) {
    pageBufferStored = false;  // Modal overlay invalidates the snapshot.
    requestUpdate();
    return;
  }
  if (sortMenu.isOpen()) {
    SortMode picked;
    if (sortMenu.handleInput(buttonNavigator, mappedInput, &picked)) {
      // Menu closed: commit the chosen sort (a no-op re-sort is skipped).
      if (picked != currentSort) {
        currentSort = picked;
        SETTINGS.sortMode = static_cast<uint8_t>(picked);
        SETTINGS.saveToFile();
        pendingSortRebuild = true;  // Render will show "Sorting…" and do the work.
        selectorIndex = 0;
        lastRenderedPage = static_cast<size_t>(-1);
      }
      pageBufferStored = false;  // Modal overlay invalidates the snapshot.
    }
    requestUpdate();  // Redraw for cursor/label changes while open, and on close.
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

#if FREEINK_DEVICE_X4PRO
  // Full Touch: a touch hold targets the cover under the finger — move the
  // selector there first so checkLongPress below (which reads the same touch
  // event and suppresses the contact) opens the menu for that cover.
  if (CrossPointSettings::FULL_TOUCH_UI) {
    int lx, ly;
    if (mappedInput.wasTouchLongPressPoint(lx, ly)) {
      const int slot = slotIndexAt(lx, ly);
      if (slot >= 0) {
        selectorIndex = currentPage() * pageSize() + static_cast<size_t>(slot);
      }
    }
  }
#endif

  // Long-press Confirm on the selected cover opens the book options modal.
  // Title/author come from currentPageMeta (populated during the page render);
  // progress is looked up in RecentBooksStore — missing means "not started".
  {
    // Index mode: the page meta (path included) and the index handle are the
    // render task's. This block reads both, so it holds the render lock for its
    // duration, taken with a try so the input loop never waits behind a page
    // render; when the lock is busy or the meta describes another page, the
    // long-press check simply runs on a later pass.
    std::optional<RenderLock> metaLock;
    if (indexMode) metaLock.emplace(RenderLock::Mode::Try);
    const bool metaUsable =
        !indexMode || (metaLock->ownsLock() && metaPage == currentPage() && selectorIndex < viewCount);
    const int slotInPage = static_cast<int>(selectorIndex - currentPage() * pageSize());
    const std::string path = !metaUsable ? std::string()
                             : indexMode ? currentPageMeta[slotInPage].path
                                         : pathAtLogicalIndex(selectorIndex);
    if (!path.empty()) {
      const std::string& title = currentPageMeta[slotInPage].title;
      const std::string& author = currentPageMeta[slotInPage].author;
      int progress = -1;
      const auto& recents = RECENT_BOOKS.getBooks();
      auto it = std::find_if(recents.begin(), recents.end(), [&path](const RecentBook& b) { return b.path == path; });
      if (it != recents.end()) progress = it->progressPercent;
      if (contextMenu.checkLongPress(mappedInput, path, title, author, progress)) {
        // Tags are short; surface them in the popup when the cached metadata has them.
        // Use the existing cache only (no rebuild) so opening the menu never blocks.
        if (indexMode) {
          // The index carries the tags; no book.bin open. Render lock is held.
          library::ClixRecord record{};
          std::string tags;
          if (index.readRecord(currentPageMeta[slotInPage].ordinal, record) && index.readGenre(record, tags)) {
            contextMenu.setInfoTags(tags);
          }
        } else if (FsHelpers::hasEpubExtension(path)) {
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

#if FREEINK_DEVICE_X4PRO
  // Full Touch: first tap on a cover moves the selector; a second tap on the
  // selected cover opens it. Selection moves stay within the current page, so
  // the framebuffer snapshot remains valid.
  if (CrossPointSettings::FULL_TOUCH_UI) {
    int lx, ly;
    if (mappedInput.wasTapPoint(lx, ly)) {
      const int slot = slotIndexAt(lx, ly);
      if (slot >= 0) {
        const size_t logical = currentPage() * pageSize() + static_cast<size_t>(slot);
        const bool activate = TouchListNav::tapActivates(static_cast<int>(logical), static_cast<int>(selectorIndex));
        selectorIndex = logical;
        pendingOpen = false;
        if (!activate) {
          requestUpdate();
        } else {
          std::string path;
          if (selectedPath(path)) {
            onSelectBook(path);
          } else if (indexMode) {
            pendingOpen = true;  // render task busy with the page; retried below
            pendingOpenIndex = selectorIndex;
          }
        }
        return;
      }
      // Dead space (header, gaps, page indicator): no action.
    }
  }
#endif

  // Short-press Confirm = open selected book.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (contextMenu.consumeLongPressFlag()) return;
    std::string path;
    if (selectedPath(path)) {
      pendingOpen = false;
      onSelectBook(path);
      return;
    }
    // Index mode: the render task holds the page right now. Remember the press
    // and open the book as soon as its path can be read (next passes).
    if (indexMode) {
      pendingOpen = true;
      pendingOpenIndex = selectorIndex;
    }
  } else if (pendingOpen) {
    std::string path;
    if (pendingOpenIndex != selectorIndex) {
      pendingOpen = false;  // the selection moved on; the press no longer applies
    } else if (selectedPath(path)) {
      pendingOpen = false;
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

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  int swipeIndex = static_cast<int>(selectorIndex);
  if (TouchListNav::pageSwipe(mappedInput, total, pageItems, swipeIndex)) {
    selectorIndex = static_cast<size_t>(swipeIndex);
    pageBufferStored = false;  // Page-jump always invalidates the snapshot.
    requestUpdate();
    return;
  }

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
