#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "components/BookContextMenu.h"
#include "components/SortMenu.h"
#include "sorting/SortMode.h"
#include "util/ButtonNavigator.h"

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
    std::string thumbPath;
  };
  std::array<SlotMeta, MAX_PAGE_SIZE> currentPageMeta;

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
  std::vector<uint32_t> dateAddedCache;
  // Parallel to bookPaths: true if the book has a BookFusion sidecar.
  // Populated once in enumerateBooks() to avoid an SD stat per cover tile, per redraw.
  std::vector<bool> bookIsBookFusion;
  bool authorCacheReady = false;
  bool dateAddedCacheReady = false;
  // Set when a sort mode change needs a metadata pass; the next render() shows a
  // "Sorting…" popup before running the (synchronous) rebuild.
  bool pendingSortRebuild = false;

  // Framebuffer snapshot cache (mirrors HomeActivity pattern at HomeActivity.cpp:134-174).
  // After a page is fully drawn, we snapshot the framebuffer so subsequent selection
  // moves only need to redraw the highlight box, skipping the expensive SD reads
  // and bitmap decodes for the 6 covers.
  uint8_t* pageBuffer = nullptr;
  bool pageBufferStored = false;
  bool pageRendered = false;
  size_t lastRenderedPage = static_cast<size_t>(-1);

  // Returns the path at the current logical position (after sort direction is applied).
  // Empty string if list is empty.
  std::string currentPath() const;
  std::string pathAtLogicalIndex(size_t logicalIdx) const;
  bool isBookFusionAtLogicalIndex(size_t logicalIdx) const;

  size_t currentPage() const { return bookPaths.empty() ? 0 : selectorIndex / pageSize(); }
  size_t totalPages() const {
    const int p = pageSize();
    return (bookPaths.size() + p - 1) / p;
  }

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
  // filled in incrementally by loop() generating one thumb per tick. Always fast.
  void renderPageFromScratch();

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

  // Framebuffer snapshot helpers — same pattern as HomeActivity.
  bool storePageBuffer();
  bool restorePageBuffer();
  void freePageBuffer();

  // Geometry helpers. Returns the on-screen Rect for the given page-local slot
  // (0..PAGE_SIZE-1). Accounts for current orientation.
  struct SlotRect {
    int x, y, width, height;
  };
  SlotRect slotRect(int slotIndexInPage) const;

  // Apply a chosen context-menu action to the book at `path`. Mirrors
  // HomeActivity::dispatchBookAction with library-flavored reload logic:
  // deletions remove from bookPaths and invalidate the page snapshot.
  void dispatchBookAction(BookContextMenu::Action action, const std::string& path, const std::string& title);
};
