#include "BookFusionBrowserActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "BookFusionTokenStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BookFusionDownload.h"
#include "network/DownloadFailureLog.h"
#include "network/HttpDownloader.h"
#include "util/TouchListNav.h"

namespace {
struct Category {
  StrId nameId;
  const char* list;
  const char* sort;
  UIIcon icon;
};

constexpr Category CATEGORIES[] = {
    {StrId::STR_BF_CURRENTLY_READING, "currently_reading", "last_read_at-desc", UIIcon::Book},
    {StrId::STR_BF_FAVORITES, "favorites", nullptr, UIIcon::Star},
    {StrId::STR_BF_PLAN_TO_READ, "planned_to_read", nullptr, UIIcon::Arrow},
    {StrId::STR_BF_COMPLETED, "completed", nullptr, UIIcon::Check},
    {StrId::STR_BF_ALL_BOOKS, nullptr, nullptr, UIIcon::Files},
};
constexpr int NUM_CATEGORIES = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

// Unified category-menu layout: [Search, categories..., shelves...]. Search sits
// at the top because it's the only row that isn't a fixed list — everything
// below it is browsable by paging, this one needs typing, so it gets the
// landing position.
constexpr int MENU_SEARCH_INDEX = 0;
constexpr int MENU_CATEGORY_BASE = MENU_SEARCH_INDEX + 1;
constexpr int MENU_SHELF_BASE = MENU_CATEGORY_BASE + NUM_CATEGORIES;

// "All Books" — no list filter, default sort. A free-text search runs against
// this category so the query is the only filter, matching the plugin, which
// opens search results with an empty filter table (bf_browser.lua
// showBookSearchDialog -> openBookList(_("Search: ") .. query, {}, query)).
constexpr int CATEGORY_ALL_BOOKS = NUM_CATEGORIES - 1;

// Longest query we'll let the user type. The buffer is fixed (searchQuery[64])
// and the keyboard enforces the cap itself, so nothing downstream truncates.
constexpr size_t MAX_SEARCH_QUERY_LEN = 63;

// The "N / M" strip both lists sit above is the theme's now
// (BaseTheme::pageIndicatorRect): drawList reserves it out of the rect it is
// handed and paints the count, so neither render() nor the page maths in
// loop() reserves anything of its own for it.

}  // namespace

void BookFusionBrowserActivity::onEnter() {
  Activity::onEnter();

  if (!BF_TOKEN_STORE.hasToken()) {
    state = ERROR;
    strlcpy(errorMsg, tr(STR_BF_NO_TOKEN_MSG), sizeof(errorMsg));
    requestUpdate();
    return;
  }

  // Connect WiFi and fetch the user's bookshelves up front so the category
  // menu shows the full [categories + shelves] list on the first render —
  // the user doesn't have to enter a category and back out to make shelves
  // appear. The wait is absorbed into the initial "Loading..." screen
  // they'd see anyway when picking any category.
  pendingWifiAction = WIFI_FOR_MENU;
  if (WiFi.status() == WL_CONNECTED) {
    loadShelvesAndShowMenu();
    return;
  }

  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionBrowserActivity::handleCategorySelection() {
  // Route selection to the search prompt, a functional category, or a user
  // shelf. The unified-menu layout (see render()) is [Search, categories,
  // shelves] — the icon changes (magnifier / category / folder) are themselves
  // the visual delimiters, so no separator rows are needed.
  if (selectedCategory == MENU_SEARCH_INDEX) {
    promptForSearch();
    return;
  }

  // Picking any category or shelf drops the previous search: the two are
  // alternative entry points, not filters that stack. searchQuery itself is
  // kept so re-opening Search still prefills the last term.
  if (selectedCategory < MENU_SHELF_BASE) {
    currentCategory = selectedCategory - MENU_CATEGORY_BASE;
    currentBookshelfId = 0;
    currentBookshelfName[0] = '\0';
  } else if (bookshelvesLoaded) {
    const int shelfIdx = selectedCategory - MENU_SHELF_BASE;
    currentBookshelfId = bookshelves.shelves[shelfIdx].id;
    strlcpy(currentBookshelfName, bookshelves.shelves[shelfIdx].name, sizeof(currentBookshelfName));
    currentCategory = -1;  // unused while a shelf filter is active
  } else {
    return;
  }
  // Not a search browse — header comes from the category/shelf and the query
  // stops filtering (searchQuery itself survives, for the next prefill).
  searchActive = false;
  searchHeader[0] = '\0';
  beginBrowse();
}

void BookFusionBrowserActivity::promptForSearch() {
  // Prefilled with the previous term so refining a search is an edit, not a
  // retype. The keyboard is pushed on top of this activity, so WiFi (and the
  // already-fetched shelf list) survive the detour.
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BF_SEARCH_BOOKS),
                                                                 searchQuery, MAX_SEARCH_QUERY_LEN, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto& kb = std::get<KeyboardResult>(result.data);
                           strlcpy(searchQuery, kb.text.c_str(), sizeof(searchQuery));
                           if (searchQuery[0] == '\0') {
                             // Cleared the box and confirmed — treat it as "cancel the search"
                             // and stay in the menu rather than fetching an unfiltered page the
                             // All Books row already offers.
                             searchActive = false;
                             searchHeader[0] = '\0';
                             return;
                           }
                           // A search is a whole-library query: drop any shelf filter and browse
                           // the unfiltered "All Books" list so the term is the only filter.
                           currentCategory = CATEGORY_ALL_BOOKS;
                           currentBookshelfId = 0;
                           currentBookshelfName[0] = '\0';
                           searchActive = true;
                           snprintf(searchHeader, sizeof(searchHeader), tr(STR_BF_SEARCH_HEADER), searchQuery);
                           beginBrowse();
                         });
}

void BookFusionBrowserActivity::beginBrowse() {
  currentPage = 1;

  if (WiFi.status() == WL_CONNECTED) {
    loadPage(1);
    return;
  }

  // WiFi dropped between activity entry and category pick — re-prompt and
  // resume into the page fetch, not the menu, after reconnecting.
  pendingWifiAction = WIFI_FOR_PAGE;
  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionBrowserActivity::onExit() {
  Activity::onExit();
  WiFi.mode(WIFI_OFF);
}

void BookFusionBrowserActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    state = ERROR;
    strlcpy(errorMsg, tr(STR_WIFI_CONN_FAILED), sizeof(errorMsg));
    requestUpdate();
    return;
  }
  if (pendingWifiAction == WIFI_FOR_MENU) {
    loadShelvesAndShowMenu();
    return;
  }
  currentPage = 1;
  loadPage(1);
}

void BookFusionBrowserActivity::loadShelvesAndShowMenu() {
  {
    RenderLock lock(*this);
    state = LOADING;
  }
  requestUpdate(true);

  // Errors are silent — the menu still shows the 5 functional categories
  // even if the shelf list fails to load (e.g. server outage). The flag flips
  // either way so we don't retry-storm.
  if (!bookshelvesLoaded) {
    {
      // Borrow the framebuffer for the TLS session. "Loading…" is already on
      // screen and nothing renders during the call, so holding the render lock
      // here is safe. See TlsFramebufferBorrow.
      TlsFramebufferBorrow borrow(renderer);
      BookFusionSyncClient::searchBookshelves(bookshelves);
    }
    bookshelvesLoaded = true;
  }

  {
    RenderLock lock(*this);
    state = CATEGORY_SELECTION;
  }
  requestUpdate();
}

void BookFusionBrowserActivity::loadPage(int page) {
  {
    RenderLock lock(*this);
    state = LOADING;
    selectedIndex = 0;
  }
  requestUpdate(true);

  // Pick the right list/sort depending on whether we're filtering by shelf or category.
  // Shelf mode bypasses the categorised lists ("currently_reading" etc.) and instead
  // sends `bookshelf_id` so the server returns books from that shelf only.
  const char* listParam = (currentBookshelfId != 0) ? nullptr : CATEGORIES[currentCategory].list;
  const char* sortParam = (currentBookshelfId != 0) ? nullptr : CATEGORIES[currentCategory].sort;
  // Resent on every page so server-side pagination stays inside the result set.
  const char* queryParam = searchActive ? searchQuery : nullptr;
  auto err = BookFusionSyncClient::NETWORK_ERROR;
  {
    // Borrow the framebuffer for the TLS session. "Loading…" is already
    // on screen; nothing renders during the call.
    TlsFramebufferBorrow borrow(renderer);
    err = BookFusionSyncClient::searchBooks(page, searchResult, listParam, sortParam, currentBookshelfId, queryParam);
  }

  if (err != BookFusionSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, BookFusionSyncClient::errorString(err), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  if (searchResult.count == 0) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, queryParam != nullptr ? tr(STR_BF_NO_SEARCH_RESULTS) : tr(STR_BF_NO_BOOKS), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  for (int i = 0; i < searchResult.count; ++i) {
    BookFusionDownload::localFilename(searchResult.books[i], downloadedFlags[i]);
  }

  {
    RenderLock lock(*this);
    state = BROWSING;
    currentPage = page;
  }
  requestUpdate();
}

void BookFusionBrowserActivity::startDownload(int bookIndex) {
  const auto& book = searchResult.books[bookIndex];

  // BookFusion shelves can contain PDF and audio books alongside EPUBs. The
  // browse list renders the non-EPUB rows with a strike-through; pressing
  // Confirm on one of them lands here. Bail with a clear message rather than
  // burning bandwidth on a file the device can't open.
  if (!BookFusionDownload::formatIsEpub(book)) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, tr(STR_BF_FORMAT_UNSUPPORTED), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = DOWNLOADING;
    downloadScreenPainted = false;      // Force a full repaint for this book's layout
    downloadTotal = book.downloadSize;  // Drives the "Filesize: … (approx. …)" line
    strlcpy(downloadTitle, book.title, sizeof(downloadTitle));
    strlcpy(downloadAuthor, book.authors[0] != '\0' ? book.authors : tr(STR_UNKNOWN_AUTHOR), sizeof(downloadAuthor));
    downloadedCoverPath[0] = '\0';  // Cleared until pre-gen succeeds below
    strlcpy(downloadStatus, tr(STR_CONNECTING), sizeof(downloadStatus));
  }
  // Non-blocking: the render task draws the "Connecting…" frame in parallel
  // with the cover fetch below. Blocking here (requestUpdateAndWait) would
  // serialise a full ~1 s e-ink refresh in front of every other step.
  requestUpdate(true);

  // Destination: per the BookFusion save-as setting (sanitized), built from the search result
  // with no network call. Downloading a book again replaces the copy already on
  // the card wherever it has been renamed or moved to, rather than adding a
  // second one under the original name.
  bool alreadyOnCard = false;
  const std::string filename = BookFusionDownload::localFilename(book, alreadyOnCard);
  // The transfer lands in a .part scratch file and is renamed on success. The
  // library browsers filter by extension, so a half-downloaded book never
  // shows up as an openable (and broken) EPUB, and a later attempt — this
  // session's retry or the user selecting the book again days later — resumes
  // it with an HTTP Range request instead of starting over.
  const std::string partPath = filename + ".part";
  const bool resumingPartial = Storage.exists(partPath.c_str());
  LOG_DBG("BFB", "Downloading book_id=%lu -> %s%s%s", static_cast<unsigned long>(book.id), filename.c_str(),
          alreadyOnCard ? " (replacing)" : "", resumingPartial ? " (resuming partial)" : "");
  BookFusionDownload::logStart(book, filename, resumingPartial);

  // Every borrow and lease in the pipeline overwrites the framebuffer, so the
  // next paint after one must be a full one, not the dynamic-band shortcut.
  BookFusionDownload::Hooks hooks;
  hooks.ctx = this;
  hooks.renderer = &renderer;
  hooks.framebufferScrambled = [](void* ctx) {
    static_cast<BookFusionBrowserActivity*>(ctx)->downloadScreenPainted = false;
  };
  hooks.transferStarting = [](void* ctx, const bool resuming) {
    auto* self = static_cast<BookFusionBrowserActivity*>(ctx);
    {
      RenderLock lock(*self);
      strlcpy(self->downloadStatus, resuming ? tr(STR_DOWNLOAD_RESUMING) : tr(STR_DOWNLOAD_WAIT),
              sizeof(self->downloadStatus));
    }
    // Paint the static info card (title / author / filesize+estimate) NOW and
    // wait for it: the e-ink holds it with no RAM, and the screen stays frozen
    // on it for the whole transfer. No progress bar — the framebuffer is lent
    // to TLS for the transfer, so nothing can render until it finishes. That
    // trade buys the CDN session the framebuffer arena for its handshake and
    // ~17KB record buffers instead of squeezing them into the browser's ~25KB
    // largest block, which is what dropped transfers mid-book.
    self->requestUpdateAndWait();
  };
  hooks.retrying = [](void* ctx) {
    auto* self = static_cast<BookFusionBrowserActivity*>(ctx);
    RenderLock lock(*self);
    strlcpy(self->downloadStatus, tr(STR_DOWNLOAD_RETRYING), sizeof(self->downloadStatus));
    self->requestUpdate(true);
  };

  // Pre-fetch the cover before the pre-signed-URL round trip. `book.coverUrl`
  // is already in memory from the search response. The cover is tiny (~30 KB)
  // relative to the EPUB; landing it on disk first lets the Downloading card
  // show it above the title. Transient CDN failures get retried; the queued
  // "Connecting…" frame just paints after a lease releases.
  Epub epub(filename, "/.crosspoint");
  epub.clearCache();
  epub.setupCacheDir();
  const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  bool apiCoverOk = BookFusionDownload::prefetchCover(book, epub, coverHeight, downloadedCoverPath,
                                                      sizeof(downloadedCoverPath), hooks);

  const auto transfer =
      BookFusionDownload::downloadEpub(book, partPath, filename, downloadUrl, sizeof(downloadUrl), hooks);
  if (transfer.urlError != BookFusionSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = ERROR;
      if (transfer.urlError == BookFusionSyncClient::NOT_FOUND) {
        strlcpy(errorMsg, tr(STR_BF_BOOK_UNAVAILABLE), sizeof(errorMsg));
      } else {
        strlcpy(errorMsg, BookFusionSyncClient::errorString(transfer.urlError), sizeof(errorMsg));
      }
    }
    requestUpdate();
    return;
  }
  if (transfer.dlResult != HttpDownloader::OK) {
    {
      RenderLock lock(*this);
      state = ERROR;
      BookFusionDownload::describeFailure(transfer.dlResult, HttpDownloader::lastHttpStatus(),
                                          Storage.exists(partPath.c_str()), errorMsg, sizeof(errorMsg));
    }
    DownloadFailureLog::line("shown=\"%s\"", errorMsg);
    requestUpdate();
    return;
  }

  // EPUB transfer done — the remaining work (metadata parse, sidecar write,
  // optional cover fallback, progress sync) is silent on the network and SD
  // bus but can take 1-2 s on a big book. Flip the status so the user knows
  // we're still doing something useful.
  strlcpy(downloadStatus, tr(STR_SAVING), sizeof(downloadStatus));
  requestUpdate(true);

  BookFusionDownload::finishDownload(book, filename, epub, coverHeight, apiCoverOk, downloadedCoverPath,
                                     sizeof(downloadedCoverPath), hooks);

  downloadedFlags[bookIndex] = true;

  {
    RenderLock lock(*this);
    state = DOWNLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void BookFusionBrowserActivity::loop() {
  if (state == WIFI_SELECTION || state == LOADING || state == DOWNLOADING) {
    return;
  }

  if (state == CATEGORY_SELECTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const bool showShelves = bookshelvesLoaded && bookshelves.count > 0;
    const int total = MENU_SHELF_BASE + (showShelves ? bookshelves.count : 0);
    // Visual page size — same calculation drawList uses internally — so a
    // long-press jump matches one screen of items exactly.
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

    // Full Touch: tap a row to select it, tap it again to open it. Same rect
    // and hasSubtitle the drawList call below uses, so the hit-test cannot
    // page differently from the menu it is testing against.
    int tappedCategory;
    switch (TouchListNav::tapRow(mappedInput, listRect(), total, selectedCategory,
                                 /*hasSubtitle=*/false, tappedCategory)) {
      case TouchListNav::TapResult::SelectionMoved:
        selectedCategory = tappedCategory;
        requestUpdate();
        return;
      case TouchListNav::TapResult::Activated:
        selectedCategory = tappedCategory;
        handleCategorySelection();
        return;
      case TouchListNav::TapResult::None:
        break;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      handleCategorySelection();
      return;
    }
    buttonNavigator.onNextRelease([this, total] {
      selectedCategory = ButtonNavigator::nextIndex(selectedCategory, total);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this, total] {
      selectedCategory = ButtonNavigator::previousIndex(selectedCategory, total);
      requestUpdate();
    });
    // Full Touch: a vertical swipe turns a page, matching the held side key.
    if (TouchListNav::pageSwipe(mappedInput, total, pageItems, selectedCategory)) {
      requestUpdate();
      return;
    }
    buttonNavigator.onNextContinuous([this, total, pageItems] {
      selectedCategory = ButtonNavigator::nextPageIndex(selectedCategory, total, pageItems);
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this, total, pageItems] {
      selectedCategory = ButtonNavigator::previousPageIndex(selectedCategory, total, pageItems);
      requestUpdate();
    });
    return;
  }

  if (state == ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (BF_TOKEN_STORE.hasToken()) {
        {
          RenderLock lock(*this);
          state = CATEGORY_SELECTION;
        }
        requestUpdate();
      } else {
        finish();
      }
    }
    return;
  }

  if (state == DOWNLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = BROWSING;
      }
      requestUpdate();
    }
    return;
  }

  if (state == CONFIRM_LARGE_DOWNLOAD) {
    // Confirm proceeds with the download; Back cancels and returns to the list.
    // wasPressed (not wasReleased) so the press that opened this screen — already
    // consumed in BROWSING — can't immediately confirm on its own release.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const int idx = pendingDownloadIndex;
      pendingDownloadIndex = -1;
      if (idx >= 0 && idx < searchResult.count) {
        startDownload(idx);
      } else {
        RenderLock lock(*this);
        state = BROWSING;
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      pendingDownloadIndex = -1;
      {
        RenderLock lock(*this);
        state = BROWSING;
      }
      requestUpdate();
    }
    return;
  }

  if (state == BROWSING) {
    const int totalItems = searchResult.count;

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state = CATEGORY_SELECTION;
      }
      requestUpdate();
      return;
    }

    int tappedIndex;
    switch (TouchListNav::tapRow(mappedInput, listRect(), searchResult.count, selectedIndex,
                                 /*hasSubtitle=*/true, tappedIndex)) {
      case TouchListNav::TapResult::SelectionMoved:
        selectedIndex = tappedIndex;
        requestUpdate();
        return;
      case TouchListNav::TapResult::Activated:
        selectedIndex = tappedIndex;
        activateSelectedBook();
        return;
      case TouchListNav::TapResult::None:
        break;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      activateSelectedBook();
      return;
    }

    // Tap = step through the current page. Falling off either end loads the
    // adjacent API page so paging works without requiring the hold-Up/Down
    // gesture. Coming back from a forward fall, the cursor lands on the
    // first item of the new page; coming back from a backward fall, it
    // lands on the last item — both keep "one step at a time" feeling
    // continuous across the page break.
    buttonNavigator.onNextRelease([this, totalItems] {
      if (selectedIndex == totalItems - 1) {
        if (searchResult.hasMore) {
          loadPage(currentPage + 1);
          return;
        }
        // On the last item of the last page — wrap forward to page 1 if we
        // know there's more than one page. Mirrors the tap-Up wrap from page 1
        // back to the last page. Lands on item 0 of page 1 so the continuous
        // step keeps going forward.
        if (currentPage > 1) {
          loadPage(1);
          return;
        }
      }
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
      requestUpdate();
    });

    buttonNavigator.onPreviousRelease([this, totalItems] {
      // First item, not on page 1 → step back a page.
      // First item, already on page 1, and we know there are more pages →
      //   wrap all the way around to the last page.
      // Either case lands on the last item of the loaded page so backward
      // stepping feels continuous across the page break.
      if (selectedIndex == 0) {
        constexpr int perPage = BookFusionSearchResult::MAX_BOOKS;
        const int lastPage = searchResult.totalCount > 0 ? (searchResult.totalCount + perPage - 1) / perPage : 0;
        int target = 0;
        if (currentPage > 1) {
          target = currentPage - 1;
        } else if (lastPage > 1) {
          target = lastPage;
        }
        if (target > 0) {
          loadPage(target);
          if (state == BROWSING && searchResult.count > 0) {
            selectedIndex = searchResult.count - 1;
            requestUpdate();
          }
          return;
        }
      }
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
      requestUpdate();
    });

    // Hold = flip to the next/previous API page. Pagination is server-side
    // (only `hasMore` is known, not the total page count), so we just bounce off
    // the ends rather than wrapping.
    const auto nextApiPage = [this] {
      if (searchResult.hasMore) {
        loadPage(currentPage + 1);
        return;
      }
      // No more pages forward — wrap to page 1 (jump semantics: land on
      // item 0). Mirrors the hold-Up wrap to the last page from page 1.
      if (currentPage > 1) {
        loadPage(1);
      }
    };

    const auto previousApiPage = [this] {
      if (currentPage > 1) {
        loadPage(currentPage - 1);
        return;
      }
      // Already on page 1 — wrap to the last page when we know how many there
      // are (Total-Count). Lands on item 0 of the last page (jump semantics)
      // rather than the last item (which is the tap-Up continuous-step
      // behaviour). If totalCount is unknown we leave hold-Up as a no-op.
      constexpr int perPage = BookFusionSearchResult::MAX_BOOKS;
      const int lastPage = searchResult.totalCount > 0 ? (searchResult.totalCount + perPage - 1) / perPage : 0;
      if (lastPage > 1) {
        loadPage(lastPage);
      }
    };

    // Full Touch: a vertical swipe flips API pages, matching the held side
    // key. Return immediately after dispatch — loadPage can leave BROWSING, so
    // the button bindings below must not run against a state we just left. The
    // lambdas' own bounds checks provide the one-page inertness pageSwipeDelta
    // deliberately doesn't (see its doc comment).
    switch (TouchListNav::pageSwipeDelta(mappedInput)) {
      case +1:
        nextApiPage();
        return;
      case -1:
        previousApiPage();
        return;
      default:
        break;
    }

    buttonNavigator.onNextContinuous(nextApiPage);
    buttonNavigator.onPreviousContinuous(previousApiPage);
  }
}

void BookFusionBrowserActivity::activateSelectedBook() {
  if (selectedIndex < searchResult.count) {
    // Gate large image-heavy EPUBs behind a confirm screen — the API gives us
    // download_size up front, so we can ask before spending the transfer. PDFs
    // and other formats are rejected inside startDownload regardless.
    const auto& book = searchResult.books[selectedIndex];
    if (BookFusionDownload::formatIsEpub(book) && BookFusionDownload::isLarge(book)) {
      pendingDownloadIndex = selectedIndex;
      {
        RenderLock lock(*this);
        state = CONFIRM_LARGE_DOWNLOAD;
      }
      requestUpdate();
    } else {
      startDownload(selectedIndex);
    }
  }
}

void BookFusionBrowserActivity::drawDownloadDynamic(const int statusY) {
  const int pageWidth = renderer.getScreenWidth();

  // Clear just the status band to white before (re)drawing it. On the full
  // path the whole screen was already cleared so this is a harmless no-op; on
  // the partial path it wipes the previous phase label (Connecting / retrying
  // / Saving differ in width) so it doesn't ghost in the retained framebuffer.
  renderer.fillRect(0, statusY, pageWidth, 40, false);

  // Phase label set by startDownload (Connecting / wait notice / Saving /
  // retrying). Falls back to the generic STR_DOWNLOADING if somehow blank.
  const char* status = (downloadStatus[0] != '\0') ? downloadStatus : tr(STR_DOWNLOADING);
  renderer.drawCenteredText(UI_10_FONT_ID, statusY, status, true, EpdFontFamily::BOLD);
}

// List body between the header and the page-indicator strip. Shared by
// render() and the loop()'s tap hit-testing so the two can never disagree, and
// by both list states — the category menu and the BROWSING book list occupy the
// same band, and differ only in whether their rows carry a subtitle.
Rect BookFusionBrowserActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // No explicit indicator reserve: drawList takes the counter strip out of the
  // rect it is handed (BaseTheme::pageIndicatorRect).
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void BookFusionBrowserActivity::render(RenderLock&&) {
  // Fast path: a progress tick on an already-painted Downloading screen. In
  // single-buffer mode the framebuffer survives between renders, so the header,
  // cover and title are still present — repaint only the dynamic status/progress
  // band instead of wiping everything and re-parsing the cover BMP from SD (the
  // SD read contends with the download's own writes on the HalStorage mutex).
  if (state == DOWNLOADING && downloadScreenPainted) {
    drawDownloadDynamic(downloadStatusY);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    // Back to normal polarity: the next tick draws over this persistent card.
    if (SETTINGS.darkMode) renderer.invertScreen();
    return;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const char* headerTitle;
  if (state == CATEGORY_SELECTION) {
    headerTitle = tr(STR_BF_BROWSE_LIBRARY);
  } else if (searchActive && searchHeader[0] != '\0') {
    // Browsing search results — header reads 'Search: <term>'.
    headerTitle = searchHeader;
  } else if (currentBookshelfId != 0) {
    // Browsing inside a user shelf — header reads the shelf's own name.
    headerTitle = currentBookshelfName;
  } else {
    headerTitle = I18N.get(CATEGORIES[currentCategory].nameId);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle);

  if (state == CATEGORY_SELECTION) {
    // Same rect the loop()'s tap hit-testing uses — see listRect().
    const Rect menuRect = listRect();

    // Unified menu layout:
    //   [MENU_SEARCH_INDEX]                 → free-text search (magnifier icon)
    //   [MENU_CATEGORY_BASE..MENU_SHELF_BASE) → functional categories (book/star/arrow/check/library icons)
    //   [MENU_SHELF_BASE..end]              → user shelves (folder icon)
    // No textual "── Bookshelves ──" separator: the icon changes between the
    // three groups are themselves the visual delimiters.
    const bool showShelves = bookshelvesLoaded && bookshelves.count > 0;
    const int total = MENU_SHELF_BASE + (showShelves ? bookshelves.count : 0);

    GUI.drawList(
        renderer, menuRect, total, selectedCategory,
        [this](int index) -> std::string {
          if (index == MENU_SEARCH_INDEX) return std::string(tr(STR_BF_SEARCH));
          if (index < MENU_SHELF_BASE) {
            return std::string(I18N.get(CATEGORIES[index - MENU_CATEGORY_BASE].nameId));
          }
          return std::string(bookshelves.shelves[index - MENU_SHELF_BASE].name);
        },
        nullptr,
        [](int index) -> UIIcon {
          if (index == MENU_SEARCH_INDEX) return UIIcon::Search;
          if (index < MENU_SHELF_BASE) return CATEGORIES[index - MENU_CATEGORY_BASE].icon;
          return UIIcon::Folder;
        },
        nullptr, true);

    // The page count is drawn by drawList itself, in the strip every screen
    // reserves for it — the category list pages by its own rows, so nothing
    // here needs to say it differently.

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == WIFI_SELECTION || state == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == ERROR) {
    // Failure reasons run to a few sentences; wrap them and centre the block
    // on the same midline a one-line message has always used.
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, errorMsg, pageWidth - 40, 6, EpdFontFamily::BOLD);
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 4;
    int y = pageHeight / 2 - (static_cast<int>(lines.size()) - 1) * lineH / 2;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += lineH;
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == CONFIRM_LARGE_DOWNLOAD) {
    const int maxWidth = pageWidth - 40;
    const bool valid = pendingDownloadIndex >= 0 && pendingDownloadIndex < searchResult.count;
    const BookFusionBook& book = searchResult.books[valid ? pendingDownloadIndex : 0];

    auto title = renderer.truncatedText(UI_10_FONT_ID, valid ? book.title : "", maxWidth);
    const auto warnLines = renderer.wrappedText(SMALL_FONT_ID, tr(STR_BF_LARGE_BOOK_WARNING), maxWidth, 4);
    const int smallH = renderer.getTextHeight(SMALL_FONT_ID) + 6;

    // Centre the title + size + warning stack around the screen midpoint.
    const int blockH = 30 + 34 + static_cast<int>(warnLines.size()) * smallH;
    int y = pageHeight / 2 - blockH / 2;

    renderer.drawCenteredText(UI_10_FONT_ID, y, title.c_str(), true, EpdFontFamily::BOLD);
    y += 30;
    char sizeText[24];
    snprintf(sizeText, sizeof(sizeText), "%.1f MB", (valid ? book.downloadSize : 0) / (1024.0f * 1024.0f));
    renderer.drawCenteredText(UI_10_FONT_ID, y, sizeText, true, EpdFontFamily::BOLD);
    y += 34;
    for (const auto& line : warnLines) {
      renderer.drawCenteredText(SMALL_FONT_ID, y, line.c_str());
      y += smallH;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DOWNLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == DOWNLOADING) {
    // Frozen info card: cover (when pre-fetched) above title / author /
    // filesize+estimate / status. No progress bar — this frame is painted once
    // and then the screen holds it while the framebuffer is lent to TLS for
    // the whole transfer (see startDownload). Only the status line repaints
    // (retrying / Saving), via the fast path at the top of render().
    constexpr int lineH = 34;
    constexpr int coverTextGap = 20;
    const int maxWidth = pageWidth - 40;
    char line[128];

    // Centre the cover + 4-line stack around the screen midpoint. Without a
    // cover this degrades to the plain centred text card.
    int y = pageHeight / 2 - 2 * lineH;
    if (downloadedCoverPath[0] != '\0') {
      FsFile coverFile;
      if (Storage.openFileForRead("BFB", downloadedCoverPath, coverFile)) {
        Bitmap bitmap(coverFile);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getHeight() > 0) {
          const int coverH = metrics.homeCoverHeight;
          const int coverW = static_cast<int>(static_cast<float>(coverH) * static_cast<float>(bitmap.getWidth()) /
                                              static_cast<float>(bitmap.getHeight()));
          const int stackH = coverH + coverTextGap + 4 * lineH;
          const int coverY = pageHeight / 2 - stackH / 2;
          renderer.drawBitmap(bitmap, (pageWidth - coverW) / 2, coverY, coverW, coverH, 0.0f);
          y = coverY + coverH + coverTextGap;
        }
        coverFile.close();
      }
    }

    snprintf(line, sizeof(line), tr(STR_DOWNLOAD_TITLE_FORMAT), downloadTitle);
    renderer.drawCenteredText(UI_10_FONT_ID, y, renderer.truncatedText(UI_10_FONT_ID, line, maxWidth).c_str(), true,
                              EpdFontFamily::BOLD);
    y += lineH;

    snprintf(line, sizeof(line), tr(STR_DOWNLOAD_AUTHOR_FORMAT), downloadAuthor);
    renderer.drawCenteredText(UI_10_FONT_ID, y, renderer.truncatedText(UI_10_FONT_ID, line, maxWidth).c_str());
    y += lineH;

    if (downloadTotal > 0) {
      // Duration estimate from the observed transfer rate (TLS decrypt + SD
      // write on the C3; measured 2.7MB in ~90s on hardware); "approx."
      // absorbs the spread.
      constexpr size_t kEstimatedBytesPerSec = 30 * 1024;
      const unsigned estSec =
          static_cast<unsigned>((downloadTotal + kEstimatedBytesPerSec - 1) / kEstimatedBytesPerSec);
      char sizeStr[16];
      snprintf(sizeStr, sizeof(sizeStr), "%.1f MB", downloadTotal / (1024.0f * 1024.0f));
      if (estSec > 90) {
        snprintf(line, sizeof(line), tr(STR_DOWNLOAD_SIZE_MIN_FORMAT), sizeStr, (estSec + 59) / 60);
      } else {
        snprintf(line, sizeof(line), tr(STR_DOWNLOAD_SIZE_SEC_FORMAT), sizeStr, estSec);
      }
      renderer.drawCenteredText(UI_10_FONT_ID, y, line);
    }
    y += lineH;

    downloadStatusY = y;           // Shared with the fast-path partial repaint
    downloadScreenPainted = true;  // Card now lives in the persistent framebuffer
    drawDownloadDynamic(y);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    // Leave the persistent card in normal polarity for the fast-path ticks.
    if (SETTINGS.darkMode) renderer.invertScreen();
    return;
  }

  if (state == DOWNLOAD_COMPLETE) {
    // If pre-gen produced a cover, draw it above the text. Centre cover + text block
    // around pageHeight/2 by shifting text down by half the cover height + gap.
    int textBaseY = pageHeight / 2;
    if (downloadedCoverPath[0] != '\0') {
      FsFile coverFile;
      if (Storage.openFileForRead("BFB", downloadedCoverPath, coverFile)) {
        Bitmap bitmap(coverFile);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getHeight() > 0) {
          const int coverH = metrics.homeCoverHeight;
          const int coverW = static_cast<int>(static_cast<float>(coverH) * static_cast<float>(bitmap.getWidth()) /
                                              static_cast<float>(bitmap.getHeight()));
          constexpr int coverTextGap = 20;
          const int coverY = pageHeight / 2 - (coverH + coverTextGap) / 2 - 15;
          renderer.drawBitmap(bitmap, (pageWidth - coverW) / 2, coverY, coverW, coverH, 0.0f);
          textBaseY = coverY + coverH + coverTextGap;
        }
        coverFile.close();
      }
    }

    renderer.drawCenteredText(UI_10_FONT_ID, textBaseY, tr(STR_BF_DOWNLOAD_COMPLETE), true, EpdFontFamily::BOLD);
    const int maxWidth = pageWidth - 40;
    auto title = renderer.truncatedText(UI_10_FONT_ID, downloadTitle, maxWidth);
    renderer.drawCenteredText(UI_10_FONT_ID, textBaseY + 30, title.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  // BROWSING state — subtitle list with title + author + BookFusion icon (matches
  // RecentBooksActivity).
  const Rect contentRect = listRect();

  // These pages are the SERVER's, not this rect's rows, so the shared counter
  // strip gets our wording. Prefer the exact count when BookFusion returned a
  // `Total-Count` response header (we read it into searchResult.totalCount in
  // the client); if the header is missing, fall back to the `+` suffix so the
  // user at least knows more pages exist.
  char indicator[24];
  if (searchResult.totalCount > 0) {
    const int perPage = BookFusionSearchResult::MAX_BOOKS;
    const int totalPages = (searchResult.totalCount + perPage - 1) / perPage;
    snprintf(indicator, sizeof(indicator), "%d / %d", currentPage, totalPages);
  } else if (searchResult.hasMore) {
    snprintf(indicator, sizeof(indicator), "%d / %d+", currentPage, currentPage);
  } else {
    snprintf(indicator, sizeof(indicator), "%d / %d", currentPage, currentPage);
  }

  GUI.drawList(
      renderer, contentRect, searchResult.count, selectedIndex,
      [this](int index) -> std::string { return std::string(searchResult.books[index].title); },
      [this](int index) -> std::string { return std::string(searchResult.books[index].authors); },
      [this](int index) { return downloadedFlags[index] ? UIIcon::Check : UIIcon::BookFusion; }, nullptr, false,
      nullptr, indicator);

  // Overlay a strike-through on rows whose book isn't an EPUB so the user can
  // see the book exists but at a glance knows it can't be opened here. The rows
  // come from the theme's own geometry (so the overlay cannot page differently
  // from the list it marks up); only the in-row line offsets (title at itemY+7,
  // subtitle at itemY+30) are replicated here, and they are the one thing to
  // update if a theme ever moves them.
  {
    const auto geo = GUI.listGeometry(contentRect, selectedIndex, /*hasSubtitle=*/true);
    const int titleStrikeY = 7 + renderer.getLineHeight(UI_10_FONT_ID) / 2;
    const int subtitleStrikeY = 30 + renderer.getLineHeight(SMALL_FONT_ID) / 2;
    const int strikeLeft = metrics.contentSidePadding + 8;
    const int strikeRight = pageWidth - metrics.contentSidePadding - 8;
    for (int i = geo.pageStart; i < searchResult.count && i < geo.pageStart + geo.pageItems; ++i) {
      if (BookFusionDownload::formatIsEpub(searchResult.books[i])) continue;
      const int itemY = contentRect.y + (i % geo.pageItems) * geo.rowHeight;
      renderer.drawLine(strikeLeft, itemY + titleStrikeY, strikeRight, itemY + titleStrikeY, true);
      renderer.drawLine(strikeLeft, itemY + subtitleStrikeY, strikeRight, itemY + subtitleStrikeY, true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
