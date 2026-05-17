#pragma once

#include <cstddef>

#include "BookFusionSyncClient.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Browse and download books from the user's BookFusion library.
 *
 * Shows the user's library 20 books at a time (paginated).
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

 private:
  enum State { CATEGORY_SELECTION, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, DOWNLOAD_COMPLETE, ERROR };

  State state = CATEGORY_SELECTION;
  ButtonNavigator buttonNavigator;

  BookFusionSearchResult searchResult;  // Current page of 20 books (~2.5 KB on heap)
  int selectedIndex = 0;
  int currentPage = 1;

  // Category menu: which item is highlighted, and which one we're browsing.
  // The menu is a unified list of [5 categories, separator, N shelves]; indices
  // map via menuIndexIsShelf() / menuIndexToShelf() below.
  int selectedCategory = 0;
  int currentCategory = 0;

  // User's custom shelves, fetched lazily on the first successful WiFi connection.
  // Loaded once per activity session — bounded at MAX_SHELVES (~1.6 KB).
  BookFusionBookshelfList bookshelves;
  bool bookshelvesLoaded = false;

  // When non-zero, the currently-displayed book list is filtered to this
  // shelf id (passed as `bookshelf_id` to /api/user/books/search). Zero means
  // a normal category-driven browse.
  uint32_t currentBookshelfId = 0;
  char currentBookshelfName[48] = {};  // header label when browsing a shelf

  // Large enough for pre-signed S3 URLs with safety margin (can be >2000 chars).
  char downloadUrl[4096] = {};
  char downloadTitle[64] = {};
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  unsigned long lastProgressUpdateMs = 0;  // Throttle progress updates
  char downloadedCoverPath[96] = {};       // Resolved thumb BMP path for DOWNLOAD_COMPLETE popup

  char errorMsg[128] = {};

  void onWifiSelectionComplete(bool success);
  void handleCategorySelection();
  void loadPage(int page);
  void startDownload(int bookIndex);
};
