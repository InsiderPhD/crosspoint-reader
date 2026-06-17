#include "RecentBooksActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>

#include <algorithm>

#include "../util/ConfirmationActivity.h"
#include "BookDetailsActivity.h"
#include "BookFusionBookIdStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long LONG_PRESS_MS = 1000;
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  recentBooksIsBookFusion.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());
  recentBooksIsBookFusion.reserve(books.size());

  for (const auto& book : books) {
    if (RecentBooksStore::isMissing(book)) continue;
    const bool isBookFusion =
        FsHelpers::hasEpubExtension(book.path) && BookFusionBookIdStore::hasBookId(book.path.c_str());
    recentBooks.push_back(book);
    recentBooksIsBookFusion.push_back(isBookFusion);
  }

  rebuildSortedIndices();
}

void RecentBooksActivity::rebuildSortedIndices() {
  std::vector<SortEntry> entries;
  entries.reserve(recentBooks.size());

  for (size_t i = 0; i < recentBooks.size(); i++) {
    const auto& b = recentBooks[i];
    SortEntry e;
    e.sortKey = b.title.empty() ? std::string_view(b.path) : std::string_view(b.title);
    e.authorKey = b.author;
    e.progressPercent = b.progressPercent;
    e.lastOpenedRank = static_cast<uint16_t>(i);  // RECENT_BOOKS order = most-recent-first
    e.dateAddedTs = 0;                            // Filled lazily in DateAdded sort modes; see below.
    entries.push_back(e);
  }

  if (currentSort == SortMode::DateAddedNewest || currentSort == SortMode::DateAddedOldest) {
    for (size_t i = 0; i < recentBooks.size(); i++) {
      HalFile f;
      if (Storage.openFileForRead("RBA", recentBooks[i].path, f)) {
        entries[i].dateAddedTs = f.getModifyDateTimePacked();
        f.close();
      }
    }
  }

  applySort(sortedIndices, entries, currentSort);
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Pull last-chosen sort from settings (shared across all list activities).
  currentSort =
      (SETTINGS.sortMode < SORT_MODE_COUNT) ? static_cast<SortMode>(SETTINGS.sortMode) : SortMode::AlphabeticAsc;

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  recentBooksIsBookFusion.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  // Power short-press → sort menu.
  if (!showingBookOptions && sortMenu.checkTrigger(mappedInput, currentSort)) {
    requestUpdate();
    return;
  }
  if (sortMenu.isOpen()) {
    SortMode picked;
    bool cancelled;
    if (sortMenu.handleInput(buttonNavigator, mappedInput, &picked, &cancelled)) {
      if (!cancelled && picked != currentSort) {
        currentSort = picked;
        SETTINGS.sortMode = static_cast<uint8_t>(picked);
        SETTINGS.saveToFile();
        rebuildSortedIndices();
      }
      requestUpdate();
    }
    return;
  }

  // Long press on a book → context menu
  if (!showingBookOptions && !longPressBookTriggered && !recentBooks.empty() && selectorIndex < sortedIndices.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 700UL) {
    const size_t actual = sortedIndices[selectorIndex];
    longPressBookTriggered = true;
    showingBookOptions = true;
    awaitingBookOptionsRelease = true;
    bookOptionsIndex = 0;
    bookOptionsPath = recentBooks[actual].path;
    bookOptionsTitle = recentBooks[actual].title;
    bookOptionsAuthor = recentBooks[actual].author;
    bookOptionsProgress = recentBooks[actual].progressPercent;
    requestUpdate();
    return;
  }

  if (showingBookOptions) {
    int optionIds[UITheme::BOOK_OPTIONS_COUNT];
    const int optionCount = UITheme::getVisibleBookOptions(optionIds, UITheme::BOOK_OPTIONS_COUNT);
    buttonNavigator.onNext([this, optionCount] {
      bookOptionsIndex = (bookOptionsIndex + 1) % optionCount;
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, optionCount] {
      bookOptionsIndex = (bookOptionsIndex - 1 + optionCount) % optionCount;
      requestUpdate();
    });

    if (awaitingBookOptionsRelease) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) awaitingBookOptionsRelease = false;
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      showingBookOptions = false;
      longPressBookTriggered = false;
      const std::string path = bookOptionsPath;
      const std::string title = bookOptionsTitle;
      const std::string author = bookOptionsAuthor;
      const int progress = bookOptionsProgress;
      const int opt = (bookOptionsIndex >= 0 && bookOptionsIndex < optionCount) ? optionIds[bookOptionsIndex] : -1;
      auto reload = [this] {
        loadRecentBooks();
        if (recentBooks.empty()) {
          selectorIndex = 0;
        } else if (selectorIndex >= static_cast<int>(recentBooks.size())) {
          selectorIndex = recentBooks.size() - 1;
        }
        requestUpdate(true);
      };
      if (opt == UITheme::BOOK_OPT_MARK_READ) {
        RECENT_BOOKS.updateProgress(path, 100);
        RECENT_BOOKS.saveToFile();
        reload();
      } else if (opt == UITheme::BOOK_OPT_RESET_PROGRESS) {
        RECENT_BOOKS.updateProgress(path, -1);
        RECENT_BOOKS.saveToFile();
        reload();
      } else if (opt == UITheme::BOOK_OPT_SHELVE) {
        RECENT_BOOKS.removeBook(path);
        RECENT_BOOKS.saveToFile();
        reload();
      } else if (opt == UITheme::BOOK_OPT_REINDEX) {
        if (FsHelpers::hasEpubExtension(path)) {
          Storage.removeDir((Epub(path, "/.crosspoint").getCachePath() + "/sections").c_str());
        } else if (FsHelpers::hasXtcExtension(path)) {
          Xtc(path, "/.crosspoint").clearCache();
        }
        reload();
      } else if (opt == UITheme::BOOK_OPT_DELETE) {
        startActivityForResult(std::make_unique<ConfirmationActivity>(
                                   renderer, mappedInput, tr(STR_DELETE_FROM_DEVICE) + std::string("?"), title),
                               [this, path, reload](const ActivityResult& res) mutable {
                                 if (!res.isCancelled) {
                                   if (FsHelpers::hasEpubExtension(path)) Epub(path, "/.crosspoint").clearCache();
                                   Storage.remove(path.c_str());
                                   RECENT_BOOKS.removeBook(path);
                                   RECENT_BOOKS.saveToFile();
                                   reload();
                                 }
                               });
      } else if (opt == UITheme::BOOK_OPT_BOOK_INFO) {
        startActivityForResult(
            std::make_unique<BookDetailsActivity>(renderer, mappedInput, path, title, author, progress),
            [this](const ActivityResult&) { requestUpdate(); });
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      showingBookOptions = false;
      longPressBookTriggered = false;
      requestUpdate();
      return;
    }
    return;  // Block main input while modal is open
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < sortedIndices.size()) {
      const size_t actual = sortedIndices[selectorIndex];
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[actual].path.c_str());
      onSelectBook(recentBooks[actual].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS),
                 sortModeLabel(currentSort));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Recent tab
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    auto at = [this](int index) -> const RecentBook& { return recentBooks[sortedIndices[index]]; };
    auto isBookFusion = [this](int index) { return recentBooksIsBookFusion[sortedIndices[index]]; };
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, sortedIndices.size(), selectorIndex,
        [at](int index) { return at(index).title; }, [at](int index) { return at(index).author; },
        [at, isBookFusion](int index) {
          return isBookFusion(index) ? UIIcon::BookFusion : UITheme::getFileIcon(at(index).path);
        },
        [at](int index) -> std::string {
          const auto& b = at(index);
          if (b.progressPercent >= 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(b.progressPercent));
            return std::string(buf);
          }
          return "";
        },
        false);
  }

  // Help text
  const auto labels = showingBookOptions
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showingBookOptions) {
    const auto lastSlash = bookOptionsPath.rfind('/');
    const std::string folder =
        (lastSlash != std::string::npos) ? bookOptionsPath.substr(0, lastSlash) : bookOptionsPath;
    UITheme::drawBookOptionsPopup(renderer, bookOptionsTitle.c_str(), bookOptionsAuthor.c_str(), folder.c_str(),
                                  bookOptionsProgress, bookOptionsIndex);
  }

  sortMenu.render(renderer);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
