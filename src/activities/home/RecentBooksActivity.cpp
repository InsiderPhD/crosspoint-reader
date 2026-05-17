#include "RecentBooksActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>

#include <algorithm>

#include "../util/ConfirmationActivity.h"
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
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());

  // First collect valid books
  std::vector<std::string> bookPaths;
  for (const auto& book : books) {
    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }
    recentBooks.push_back(book);
    bookPaths.push_back(book.path);
  }

  // Apply sorting to the paths
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);


  // Long press on a book → context menu
  if (!showingBookOptions && !longPressBookTriggered && !recentBooks.empty() &&
      selectorIndex < static_cast<int>(recentBooks.size()) &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 700UL) {
    longPressBookTriggered = true;
    showingBookOptions = true;
    awaitingBookOptionsRelease = true;
    bookOptionsIndex = 0;
    bookOptionsPath = recentBooks[selectorIndex].path;
    bookOptionsTitle = recentBooks[selectorIndex].title;
    bookOptionsAuthor = recentBooks[selectorIndex].author;
    bookOptionsProgress = recentBooks[selectorIndex].progressPercent;
    requestUpdate();
    return;
  }

  if (showingBookOptions) {
    buttonNavigator.onNext([this] {
      bookOptionsIndex = (bookOptionsIndex + 1) % UITheme::BOOK_OPTIONS_COUNT;
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      bookOptionsIndex = (bookOptionsIndex - 1 + UITheme::BOOK_OPTIONS_COUNT) % UITheme::BOOK_OPTIONS_COUNT;
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
      auto reload = [this] {
        loadRecentBooks();
        if (recentBooks.empty()) {
          selectorIndex = 0;
        } else if (selectorIndex >= static_cast<int>(recentBooks.size())) {
          selectorIndex = recentBooks.size() - 1;
        }
        requestUpdate(true);
      };
      if (bookOptionsIndex == UITheme::BOOK_OPT_MARK_READ) {
        RECENT_BOOKS.updateProgress(path, 100);
        RECENT_BOOKS.saveToFile();
        reload();
      } else if (bookOptionsIndex == UITheme::BOOK_OPT_RESET_PROGRESS) {
        RECENT_BOOKS.updateProgress(path, -1);
        RECENT_BOOKS.saveToFile();
        reload();
      } else if (bookOptionsIndex == UITheme::BOOK_OPT_SHELVE) {
        RECENT_BOOKS.removeBook(path);
        RECENT_BOOKS.saveToFile();
        reload();
      } else if (bookOptionsIndex == UITheme::BOOK_OPT_REINDEX) {
        if (FsHelpers::hasEpubExtension(path)) {
          Storage.removeDir((Epub(path, "/.crosspoint").getCachePath() + "/sections").c_str());
        } else if (FsHelpers::hasXtcExtension(path)) {
          Xtc(path, "/.crosspoint").clearCache();
        }
        reload();
      } else if (bookOptionsIndex == UITheme::BOOK_OPT_DELETE) {
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                   tr(STR_DELETE_FROM_DEVICE) + std::string("?"), title),
            [this, path, reload](const ActivityResult& res) mutable {
              if (!res.isCancelled) {
                if (FsHelpers::hasEpubExtension(path)) Epub(path, "/.crosspoint").clearCache();
                Storage.remove(path.c_str());
                RECENT_BOOKS.removeBook(path);
                RECENT_BOOKS.saveToFile();
                reload();
              }
            });
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
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
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

  // Show current sort method in header
  // TODO: Temporarily disabled to fix crash
  /*char headerTitle[128];
  snprintf(headerTitle, sizeof(headerTitle), "%s (%s)",
           tr(STR_MENU_RECENT_BOOKS), sortableFiles.getCurrentSortDisplayName());

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle);*/
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Recent tab
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; }, [this](int index) { return recentBooks[index].author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); },
        [this](int index) -> std::string {
          if (recentBooks[index].progressPercent >= 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(recentBooks[index].progressPercent));
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

  // Render sort indicator overlay if active
  // TODO: Temporarily disabled to fix crash
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
