#include "LibraryActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <memory>
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
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    if (SETTINGS.darkMode) renderer.invertScreen();

    // Preempt the background boot-time NTP sync (if still running) so its WiFi
    // stack heap is freed before the metadata recache's heavy allocations —
    // avoids a cold-boot OOM/contention crash. No-op after the ~10s boot window.
    WifiTimeSync::preempt();
    {
      const uint32_t t0 = millis();
      enumerateBooks();
      LOG_INF("LIBTIME", "enumerate+sort %u books: %lu ms", static_cast<unsigned>(bookPaths.size()),
              static_cast<unsigned long>(millis() - t0));
    }
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

  if (bookPaths.empty()) {
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
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookPaths.size() - pageStart));

  // Reset all slots so stale data from a previous page doesn't leak.
  for (auto& m : currentPageMeta) {
    m.title.clear();
    m.author.clear();
    m.progressPercent = -1;
    m.hasCover = false;
    m.hasBfBadge = false;
    m.coverResolved = false;
    m.thumbPath.clear();
  }

  for (int i = 0; i < booksOnPage; ++i) {
    const uint32_t t0 = millis();
    const std::string path = pathAtLogicalIndex(pageStart + i);
    auto& meta = currentPageMeta[i];

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
      if (epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
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
}

int LibraryActivity::findMissingThumbSlot() const {
  const size_t page = currentPage();
  const size_t pageStart = page * pageSize();
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookPaths.size() - pageStart));
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
  meta.author = epub.getAuthor();

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
  const int booksOnPage = static_cast<int>(std::min<size_t>(pageSize(), bookPaths.size() - pageStart));

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

  // Page count, in the strip every scrollable screen reserves for it: the grid
  // pages the same way a list does, so it says so the same way. The "loading vs
  // broken" distinction is communicated via the centered Loading popup that
  // renderPageFromScratch shows during active generation.
  const int ps = pageSize();
  const size_t pages = (bookPaths.size() + ps - 1) / ps;
  // The rect the grid was laid out in, ending where the hint bar starts —
  // drawPageIndicator takes its strip out of the bottom of that.
  const Rect gridRect{0, L.topY, screenW, screenH - metrics.buttonHintsHeight - L.topY};
  GUI.drawPageIndicator(renderer, gridRect, static_cast<int>(page) + 1, static_cast<int>(pages));

  // Scrollbar on the right edge, alongside the grid. Position + thumb height
  // matches LyraTheme::drawList (LyraTheme.cpp:236-245): vertical track line +
  // filled rect proxy for current page within total pages. Shown only when
  // there's more than one page.
  const int totalP = static_cast<int>(pages);
  const int totalItems = static_cast<int>(bookPaths.size());
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
  const int total = static_cast<int>(bookPaths.size());
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
  if (bookPaths.empty()) return;

  const int total = static_cast<int>(bookPaths.size());
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
        if (!activate) {
          requestUpdate();
        } else {
          const std::string path = currentPath();
          if (!path.empty()) {
            onSelectBook(path);
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
