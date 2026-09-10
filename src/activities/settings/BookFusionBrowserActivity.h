#pragma once

#include <cstddef>

#include "BookFusionSyncClient.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * Browse and download books from the user's BookFusion library.
 *
 * Shows the user's library 10 books at a time (paginated).
 * Selecting a book fetches a pre-signed download URL, streams the EPUB
 * to the SD card, and writes a BookFusion sidecar via BookFusionBookIdStore
 * so that progress sync works immediately after download.
 *
 * Requires a linked BookFusion account (token in BF_TOKEN_STORE).
 */
class BookFusionBrowserActivity final : public Activity {
 public:
  explicit BookFusionBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookFusionBrowser", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  // Full Touch tap dispatch covers the two list states — the category menu and
  // the BROWSING book list — both of which hit-test taps against listRect().
  // The remaining states (confirm / downloading / complete / error) are single
  // prompts with no rows to aim at, so they keep the global tap-is-Confirm
  // injection, which activates the highlighted option.
  bool handlesDirectTouch() const override { return state == BROWSING || state == CATEGORY_SELECTION; }

 private:
  enum State {
    CATEGORY_SELECTION,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    CONFIRM_LARGE_DOWNLOAD,
    DOWNLOADING,
    DOWNLOAD_COMPLETE,
    ERROR
  };

  State state = CATEGORY_SELECTION;
  ButtonNavigator buttonNavigator;

  BookFusionSearchResult searchResult;                           // Current page of 10 books plus cover URLs (~5 KB)
  bool downloadedFlags[BookFusionSearchResult::MAX_BOOKS] = {};  // true if the book file already exists on SD
  int selectedIndex = 0;
  int currentPage = 1;

  // Category menu: which item is highlighted, and which one we're browsing.
  // The menu is a unified list of [Search, 5 categories, N shelves]; indices
  // map via the MENU_* bases in the .cpp.
  int selectedCategory = 0;
  int currentCategory = 0;

  // User's custom shelves, fetched up front in onEnter() once WiFi is available
  // so the unified menu (categories + shelves) is fully populated before the user
  // picks anything. Bounded at MAX_SHELVES (~1.6 KB on the activity).
  BookFusionBookshelfList bookshelves;
  bool bookshelvesLoaded = false;

  // What to do after WiFi connects: open the category menu (entry flow) or
  // jump straight to fetching a page (user already picked a category).
  enum PendingWifiAction { WIFI_FOR_MENU, WIFI_FOR_PAGE };
  PendingWifiAction pendingWifiAction = WIFI_FOR_MENU;

  // When non-zero, the currently-displayed book list is filtered to this
  // shelf id (passed as `bookshelf_id` to /api/user/books/search). Zero means
  // a normal category-driven browse.
  uint32_t currentBookshelfId = 0;
  char currentBookshelfName[48] = {};  // header label when browsing a shelf

  // Free-text search over the whole library, sent as `query` to
  // /api/user/books/search. Empty means "not searching". Kept between visits to
  // the menu so re-opening Search prefills the last query (the KOReader plugin
  // does the same), and re-sent on every page fetch so pagination stays in the
  // result set.
  char searchQuery[64] = {};
  char searchHeader[80] = {};  // Pre-formatted 'Search: foo' header label
  // Whether searchQuery is actually filtering the current browse. Separate from
  // searchQuery being non-empty: the term is deliberately retained after the
  // user leaves the results (so re-opening Search prefills it) but must NOT be
  // sent once they pick a category or shelf instead.
  bool searchActive = false;

  // Large enough for pre-signed S3 URLs with safety margin (can be >2000 chars).
  char downloadUrl[4096] = {};
  char downloadTitle[64] = {};
  char downloadAuthor[64] = {};       // For the frozen Downloading info card
  size_t downloadTotal = 0;           // Expected size (API download_size) for the Filesize line
  char downloadedCoverPath[96] = {};  // Resolved thumb BMP path for DOWNLOAD_COMPLETE popup
  int pendingDownloadIndex = -1;      // Book awaiting CONFIRM_LARGE_DOWNLOAD acceptance
  // Phase label shown on the Downloading screen — "Connecting…" → "Downloading…"
  // → "Saving…". Updated at each long-running step so the user can see the activity
  // isn't stuck. Empty string falls back to the generic Downloading label.
  char downloadStatus[32] = {};

  // The Downloading screen re-renders on every ~2 s progress tick. A full
  // render() wipes the framebuffer and re-parses the cover BMP from SD — wasteful
  // SD churn that contends with the download's own writes on the shared
  // HalStorage mutex, and we have no spare heap to cache the decoded cover
  // (the transfer already runs at ~16 KB free; see HttpDownloader.cpp). Instead
  // we exploit single-buffer mode: the framebuffer persists between renders, so
  // once the cover/title are painted we repaint only the dynamic status/progress
  // band on each tick and leave the static pixels untouched.
  bool downloadScreenPainted = false;  // Full Downloading layout already in the framebuffer
  int downloadStatusY = 0;             // Y of the status line, shared by full + partial repaint
  void drawDownloadDynamic(int statusY);

  char errorMsg[128] = {};

  // List body shared by both list states (category menu and BROWSING book
  // list) and by render() + the loop()'s tap hit-testing, so no two of them
  // can disagree about where the rows are.
  Rect listRect() const;
  // Download the highlighted book (via the large-download confirm gate) — the
  // Confirm press body, also fired by a Full Touch tap on the selected row.
  void activateSelectedBook();
  void onWifiSelectionComplete(bool success);
  void handleCategorySelection();
  // Prompt for a search term on the keyboard, then browse the results.
  void promptForSearch();
  // Fetch page 1 of whatever category / shelf / query is currently selected,
  // re-prompting for WiFi first if it dropped while we were in the menu.
  void beginBrowse();
  void loadShelvesAndShowMenu();
  void loadPage(int page);
  void startDownload(int bookIndex);
};
