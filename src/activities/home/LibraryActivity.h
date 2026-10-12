#pragma once

#include <LibraryIndexFile.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "components/BookContextMenu.h"
#include "components/SortMenu.h"
#include "sorting/SortMode.h"
#include "util/ButtonNavigator.h"

namespace library {
struct BuildProgress;
}

class LibraryActivity final : public Activity {
 public:
  static constexpr int COVER_HEIGHT = 226;
  static constexpr int GRID_COLS = 3;
  static constexpr int MAX_GRID_ROWS = 2;
  static constexpr int MAX_PAGE_SIZE = GRID_COLS * MAX_GRID_ROWS;  // Caps currentPageMeta sizing.

  // Orientation-dependent grid shape. Portrait fits the full 2 rows × 226-tall
  // Lyra3Covers cover; landscape can only fit 1 row at that height, so the
  // grid collapses to a single row of 3.
  int gridRows() const;
  int pageSize() const { return GRID_COLS * gridRows(); }

  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}
  ~LibraryActivity() override = default;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // While the sort/context menus are open, taps belong to them (inside-row
  // hit-testing + injected Confirm); the grid resumes tap ownership on close.
  bool handlesDirectTouch() const override { return !contextMenu.isOpen() && !sortMenu.isOpen(); }
  // A first scan of a large card can outlast the sleep timeout; the device must
  // not go to sleep (and block on the render lock) in the middle of one.
  bool preventAutoSleep() override { return scanInProgress.load(); }

  // Delete the persisted library index so the next library open re-scans the SD
  // card. Call after adding or removing a book from outside the library (e.g. a
  // BookFusion/OPDS download): the index's directory-mtime validation can't detect
  // a file added to the FAT root, so without this the new book stays invisible in
  // the library until the cache happens to be invalidated for another reason.
  static void invalidateIndexCache();

 private:
  ButtonNavigator buttonNavigator;
  BookContextMenu contextMenu;

  // Bare-minimum per-book state. Paths only — title/author/thumb are derived
  // lazily during page render via the per-book .crosspoint cache.
  std::vector<std::string> bookPaths;

  // Title + author for the books currently visible on the active page. Filled
  // by renderPageFromScratch (the same pass that lazy-generates thumbs) so the
  // book context menu can open instantly on long-press without re-parsing
  // metadata from disk. Indexed by page-local slot (0..PAGE_SIZE-1).
  struct SlotMeta {
    std::string title;
    std::string author;
    int progressPercent = -1;  // -1 when the book has no RecentBooksStore entry.
    bool hasCover = false;
    bool hasBfBadge = false;
    // drawTileCover has tried to open thumbPath (and the BookFusion cover.bmp
    // fallback) and fixed up hasCover/thumbPath from the result. Cleared by
    // refreshCurrentPageMeta, which no longer probes the SD for covers itself.
    bool coverResolved = false;
    std::string thumbPath;
    // Index mode only: the book's full path and its record ordinal in the CLX1
    // index, so loop() can act on the selected book without touching the index
    // file the render task reads from.
    std::string path;
    uint16_t ordinal = 0xFFFF;
  };
  std::array<SlotMeta, MAX_PAGE_SIZE> currentPageMeta;

  // Two-phase cover loader bookkeeping: which page-local slots we've already run
  // generateThumbForSlot() on during the current page visit. Unlike SlotMeta's
  // hasCover (which refreshCurrentPageMeta re-derives from disk every render),
  // this MUST survive the per-render meta refresh — otherwise a book whose cover
  // fails to generate (Epub::generateThumbBmp removes the thumb on a failed
  // JPEG/PNG convert) is re-selected by findMissingThumbSlot every render, wedging
  // the loader on that one slot forever (endless "Loading…" flashing, no covers).
  // Cleared in renderPageFromScratch only when the page actually changes.
  std::array<bool, MAX_PAGE_SIZE> coverGenAttempted{};

  // Pagination + selection state. selectorIndex is a *logical* index into the
  // sorted (live) list, not a page-local index — this keeps wrap-around free
  // via ButtonNavigator's nextIndex / nextPageIndex helpers.
  size_t selectorIndex = 0;

  SortMenu sortMenu;
  SortMode currentSort = SortMode::AlphabeticAsc;
  std::vector<uint16_t> sortedIndices;
  // Lazy metadata caches, populated on first selection of the relevant sort mode.
  // Indexed in parallel with bookPaths (raw enumeration order).
  std::vector<std::string> authorCache;
  std::vector<std::string> tagCache;
  std::vector<uint32_t> dateAddedCache;
  std::vector<bool> bfBadgeCache;
  bool authorCacheReady = false;
  bool tagCacheReady = false;
  bool dateAddedCacheReady = false;
  bool bfBadgeCacheReady = false;
  // Set when a sort mode change needs a metadata pass; the next render() shows a
  // "Sorting…" popup before running the (synchronous) rebuild.
  bool pendingSortRebuild = false;
  // Set on activity entry; the first render() shows a "Loading…" popup before
  // running enumerateBooks(). The SD-card BFS dominates initial load time on
  // libraries with many files, so the user needs feedback that the device
  // hasn't frozen.
  bool initialLoadPending = false;

  // Framebuffer snapshot cache (mirrors HomeActivity pattern at HomeActivity.cpp:134-174).
  // After a page is fully drawn, we snapshot the framebuffer so subsequent selection
  // moves only need to redraw the highlight box, skipping the expensive SD reads
  // and bitmap decodes for the 6 covers.
  uint8_t* pageBuffer = nullptr;
  bool pageBufferStored = false;
  // On the C3 the snapshot malloc (52KB) fails whenever the library is
  // populated (largest free block ~45KB after the path list is loaded), so the
  // fast paths below cannot depend on it:
  // - frameHoldsPage: the framebuffer still holds this page's covers. The
  //   selection band never overlaps a cover, so a cursor move repaints only the
  //   chrome of the two affected tiles (clearTileChrome + drawOverlay).
  // - frameOverlayed: the last render painted a menu or popup over the page;
  //   the next menu-less render must rebuild it unless a snapshot restores it.
  // - lastDrawnSelector: logical index whose tile last received the band.
  bool frameHoldsPage = false;
  bool frameOverlayed = false;
  size_t lastDrawnSelector = static_cast<size_t>(-1);
  size_t lastRenderedPage = static_cast<size_t>(-1);

  // Returns the path at the current logical position (after sort direction is applied).
  // Empty string if list is empty.
  // Render-task only in index mode (it reads the index file).
  std::string pathAtLogicalIndex(size_t logicalIdx);

  // Books in the displayed list, whichever backing store holds them.
  size_t bookCount() const { return indexMode ? viewCount : bookPaths.size(); }
  size_t currentPage() const { return bookCount() == 0 ? 0 : selectorIndex / pageSize(); }
  size_t totalPages() const {
    const int p = pageSize();
    return (bookCount() + p - 1) / p;
  }

  // --- CLX1 index backing (SETTINGS.libraryUseIndex) -------------------------
  //
  // With the index, nothing per-book is held as a string: bookPaths and the
  // author/tag/date caches above stay empty. What is resident while the Library
  // is open is the index file handle (a few hundred bytes), `view` (2 bytes per
  // book), an optional 1-bit-per-book hidden set and a 512-byte folder offset
  // table. All of it is released in onExit(); nothing outlives the activity.
  //
  // Threading: the index file handle, `view` and currentPageMeta are owned by
  // the render task. loop() reaches them only through selectedPath(), which
  // takes the render lock with a try (never blocking the input loop).
  library::LibraryIndexFile index;
  bool indexMode = false;
  // Display row -> record ordinal for the current sort, hidden books removed.
  std::unique_ptr<uint16_t[]> view;
  uint16_t viewCount = 0;
  // By record ordinal: books deleted during this visit (the index on the card
  // still lists them until the next scan). Null until first needed.
  std::unique_ptr<uint8_t[]> hiddenBits;
  // Sparse folder-record offsets so a path read does not walk every folder.
  std::unique_ptr<uint32_t[]> folderOffsets;
  uint16_t folderStride = 0;
  // Page currentPageMeta describes; npos while it is being rebuilt.
  size_t metaPage = static_cast<size_t>(-1);
  // Mutations requested by loop()/activity results, applied by the next render.
  bool pendingViewRebuild = false;
  uint16_t pendingHideOrdinal = 0xFFFF;
  // Confirm/tap arrived while the render task held the page meta; retried.
  bool pendingOpen = false;
  size_t pendingOpenIndex = 0;
  // Scan in progress on the render task. loop() keeps running on the main task
  // and turns a Back release into a cancel request the builder polls.
  std::atomic<bool> scanInProgress{false};
  std::atomic<bool> scanCancelRequested{false};
  // A cancelled first scan leaves no index to show: go Home from loop().
  std::atomic<bool> exitRequested{false};

  // Load the book list from whichever backing store is usable.
  void loadLibrary();
  // Validate (and if needed rebuild, with progress + cancel) the CLX1 index,
  // open it and build the view. False = use the in-RAM path list instead.
  bool openIndexBacking();
  void closeIndexBacking();
  // Recompute `view` for currentSort. Keeps the previous view on failure.
  bool rebuildView();
  bool indexedPath(uint16_t ordinal, std::string& out);
  // Selected book's path for loop(): page meta when current, else the index,
  // both under a try-lock in index mode. False = nothing selected or busy.
  bool selectedPath(std::string& out);
  void drawScanScreen(const char* message);
  static bool scanCancelCallback(void* context);
  static void scanProgressCallback(void* context, const library::BuildProgress& progress);
  static bool siblingPathCallback(void* context, int pos, std::string& out);

  // Enumerate every .epub / .xtc on the SD (recursive, iterative via queue).
  // Order: directory enumeration order (insertion order on FAT/exFAT). The
  // user-visible order is determined by `sortedIndices`, populated by
  // rebuildSortedIndices().
  void enumerateBooks();

  // Recomputes `sortedIndices` from bookPaths + the current sort mode. May
  // populate authorCache or dateAddedCache the first time those modes are used.
  void rebuildSortedIndices();

  // Render the current page from scratch using whatever metadata + thumb caches
  // exist on disk *right now*. Missing covers render as placeholders; they get
  // filled in one at a time by render() -> fillMissingCover(). Always fast.
  void renderPageFromScratch();

  // Clear a tile's selection strips and title box (everything but the cover)
  // so drawOverlay() can repaint the band and text without a snapshot.
  void clearTileChrome(int slot);

  // Repopulates currentPageMeta from on-disk caches for the current page.
  // Called by both renderPageFromScratch and after a successful thumb-gen tick
  // (so the new title/author/has-cover state shows up on the next render).
  void refreshCurrentPageMeta();

  // Generates the thumbnail for one page-local slot (sync, ~1-5s on first hit).
  // Returns true if it actually ran a slow generation step (i.e. the slot was
  // missing a thumb before). Returns false if there was nothing to do.
  bool generateThumbForSlot(int slotIndexInPage);

  // Returns the page-local slot index of the first slot on the current page
  // missing a thumb, or -1 if everything is cached.
  int findMissingThumbSlot() const;

  // Generates one missing cover, then paints just that tile into the page
  // snapshot (the rest of the page is already there) and schedules a render.
  // Called by render() only AFTER it has pushed the placeholder page + popup.
  void fillMissingCover(int slotIndexInPage);

  // Cover bitmap (or placeholder) + border + BookFusion badge for one slot.
  void drawTileCover(int slotIndexInPage);

  // Restore the cached framebuffer, then redraw the per-frame overlay (titles,
  // selection bands, page indicator). Fast — no SD reads, no decodes.
  void renderSelectionOnly();

  // Per-frame overlay drawn on top of the cover layer. Mirrors the second-pass
  // section of Lyra3CoversTheme::drawRecentBookCover: selection bands first
  // (under text) then title text. Adds a centered page indicator at the bottom.
  void drawOverlay();

  // Bottom-of-screen button hint bar (« Home / Open / Up / Down). Labels swap
  // to (Back / Select / Up / Down) while the book context menu is open.
  void drawButtonHints();

  // Framebuffer snapshot helpers — same pattern as HomeActivity. The 48KB
  // buffer is allocated on the first store and reused until onExit.
  bool storePageBuffer();
  bool restorePageBuffer();
  void freePageBuffer();

  // Geometry helpers. Returns the on-screen Rect for the given page-local slot
  // (0..PAGE_SIZE-1). Accounts for current orientation.
  struct SlotRect {
    int x, y, width, height;
  };
  SlotRect slotRect(int slotIndexInPage) const;
  // Page-local slot under a logical-frame point (Full Touch tap dispatch), or
  // -1 for a miss / an empty trailing slot on the last page.
  int slotIndexAt(int lx, int ly) const;

  // Apply a chosen context-menu action to the book at `path`. Mirrors
  // HomeActivity::dispatchBookAction with library-flavored reload logic:
  // deletions remove from bookPaths and invalidate the page snapshot.
  void dispatchBookAction(BookContextMenu::Action action, const std::string& path, const std::string& title);
};
