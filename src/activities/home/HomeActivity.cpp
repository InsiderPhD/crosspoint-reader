#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "../util/ConfirmationActivity.h"
#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 5;  // File Browser, Recents, File transfer, Stats, Settings
  count += getCoverSlotsUsed();
  if (hasOpdsUrl) {
    count++;
  }
  return count;
}

int HomeActivity::getCoverSlotsUsed() const {
  const int librarySlot = UITheme::getInstance().getTheme().getLibrarySlotIndex();
  const int recents = static_cast<int>(recentBooks.size());
  // Library tile is always reachable: at minimum we expose librarySlot+1 slots.
  return librarySlot >= 0 ? std::max(recents, librarySlot + 1) : recents;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;

  selectorIndex = 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  // When the active theme reserves a library tile, only load enough recents
  // to fill the slots before it — slots at/after librarySlot are not books.
  const int librarySlot = UITheme::getInstance().getTheme().getLibrarySlotIndex();
  const int maxRecents = librarySlot >= 0 ? librarySlot : metrics.homeRecentBooksCount;
  loadRecentBooks(maxRecents);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = renderer.getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  const size_t bufferSize = renderer.getBufferSize();
  memcpy(frameBuffer, coverBuffer, bufferSize);
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}

void HomeActivity::dispatchBookAction(BookContextMenu::Action action, const std::string& path,
                                      const std::string& title) {
  // Reload the recent-books vector after any mutation, keeping selector + cover
  // cache state in sync. Captures `this` to access members.
  auto reloadRecents = [this] {
    const auto& m = UITheme::getInstance().getMetrics();
    const int librarySlot = UITheme::getInstance().getTheme().getLibrarySlotIndex();
    const int maxRecents = librarySlot >= 0 ? librarySlot : m.homeRecentBooksCount;
    recentBooks.clear();
    loadRecentBooks(maxRecents);
    selectorIndex = 0;
    recentsLoaded = false;
    recentsLoading = false;
    coverRendered = false;
    coverBufferStored = false;
    requestUpdate();
  };

  switch (action) {
    case BookContextMenu::Action::MarkRead:
      RECENT_BOOKS.updateProgress(path, 100);
      RECENT_BOOKS.saveToFile();
      reloadRecents();
      break;
    case BookContextMenu::Action::ResetProgress:
      RECENT_BOOKS.updateProgress(path, -1);
      RECENT_BOOKS.saveToFile();
      reloadRecents();
      break;
    case BookContextMenu::Action::Shelve:
      RECENT_BOOKS.removeBook(path);
      RECENT_BOOKS.saveToFile();
      reloadRecents();
      break;
    case BookContextMenu::Action::Reindex:
      if (FsHelpers::hasEpubExtension(path)) {
        // Delete only rendered sections — preserves progress, covers, and metadata.
        const std::string sectionsPath = Epub(path, "/.crosspoint").getCachePath() + "/sections";
        Storage.removeDir(sectionsPath.c_str());
      } else if (FsHelpers::hasXtcExtension(path)) {
        Xtc(path, "/.crosspoint").clearCache();
      }
      reloadRecents();
      break;
    case BookContextMenu::Action::Delete:
      startActivityForResult(std::make_unique<ConfirmationActivity>(
                                 renderer, mappedInput, tr(STR_DELETE_FROM_DEVICE) + std::string("?"), title),
                             [this, path, reloadRecents](const ActivityResult& res) mutable {
                               if (!res.isCancelled) {
                                 if (FsHelpers::hasEpubExtension(path)) {
                                   Epub(path, "/.crosspoint").clearCache();
                                 }
                                 Storage.remove(path.c_str());
                                 RECENT_BOOKS.removeBook(path);
                                 RECENT_BOOKS.saveToFile();
                                 reloadRecents();
                               }
                             });
      break;
    case BookContextMenu::Action::RegenerateCover:
      LOG_DBG("HAC", "Manual cover regeneration requested for: %s", path.c_str());
      if (FsHelpers::hasEpubExtension(path)) {
        Epub epub(path, "/.crosspoint");
        if (epub.load(false, true)) {  // buildIfMissing=false, skipLoadingCss=true
          const auto& metrics = UITheme::getInstance().getMetrics();
          const int coverHeight = metrics.homeCoverHeight;
          std::string coverPath226 = epub.getThumbBmpPath(coverHeight);
          if (Storage.exists(coverPath226.c_str())) {
            Storage.remove(coverPath226.c_str());
            LOG_DBG("HAC", "Removed existing cover: %s", coverPath226.c_str());
          }
          if (epub.generateThumbBmp(coverHeight)) {
            RECENT_BOOKS.updateBook(path, epub.getTitle(), epub.getAuthor(), coverPath226);
            LOG_DBG("HAC", "Cover regeneration successful: %s", coverPath226.c_str());
          } else {
            LOG_ERR("HAC", "Cover regeneration failed for: %s", path.c_str());
          }
        } else {
          LOG_ERR("HAC", "Failed to load EPUB metadata for cover regeneration: %s", path.c_str());
        }
      } else if (FsHelpers::hasXtcExtension(path)) {
        Xtc xtc(path, "/.crosspoint");
        if (xtc.load()) {
          const auto& metrics = UITheme::getInstance().getMetrics();
          const int coverHeight = metrics.homeCoverHeight;
          std::string coverPath = xtc.getThumbBmpPath(coverHeight);
          if (Storage.exists(coverPath.c_str())) {
            Storage.remove(coverPath.c_str());
            LOG_DBG("HAC", "Removed existing XTC cover: %s", coverPath.c_str());
          }
          if (xtc.generateThumbBmp(coverHeight)) {
            RECENT_BOOKS.updateBook(path, xtc.getTitle(), xtc.getAuthor(), coverPath);
            LOG_DBG("HAC", "XTC cover regeneration successful: %s", coverPath.c_str());
          } else {
            LOG_ERR("HAC", "XTC cover regeneration failed for: %s", path.c_str());
          }
        }
      }
      RECENT_BOOKS.saveToFile();
      reloadRecents();
      break;
  }
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

  // Active modal — drive it and dispatch the chosen action (if any) on a terminal event.
  if (contextMenu.isOpen()) {
    BookContextMenu::Action action;
    bool cancelled = false;
    if (contextMenu.handleInput(buttonNavigator, mappedInput, &action, &cancelled)) {
      if (!cancelled) {
        dispatchBookAction(action, contextMenu.path(), contextMenu.title());
      } else {
        requestUpdate();
      }
    } else {
      // Modal still up — selection navigation might have changed; redraw.
      requestUpdate();
    }
    return;
  }

  // Long-press Confirm on a recent-book tile opens the book options modal.
  if (selectorIndex < static_cast<int>(recentBooks.size())) {
    const auto& book = recentBooks[selectorIndex];
    if (contextMenu.checkLongPress(mappedInput, book.path, book.title, book.author, book.progressPercent)) {
      requestUpdate();
      return;
    }
  }

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // If this release was the menu's confirmation/dismissal, the helper has
    // already handled it — skip short-press handling.
    if (contextMenu.consumeLongPressFlag()) return;

    // Library slot is checked first so a theme-provided library tile beats
    // both the recent-book lookup and the menu fallthrough.
    const int librarySlotIdx = UITheme::getInstance().getTheme().getLibrarySlotIndex();
    if (librarySlotIdx >= 0 && selectorIndex == librarySlotIdx) {
      activityManager.goToLibrary();
      return;
    }

    const int coverSlotsUsed = getCoverSlotsUsed();

    // Calculate dynamic indices based on which options are available
    int idx = 0;
    int menuSelectedIndex = selectorIndex - coverSlotsUsed;
    const int fileBrowserIdx = idx++;
    const int recentsIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int statsIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else if (menuSelectedIndex == fileBrowserIdx) {
      onFileBrowserOpen();
    } else if (menuSelectedIndex == recentsIdx) {
      onRecentsOpen();
    } else if (menuSelectedIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (menuSelectedIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (menuSelectedIndex == statsIdx) {
      onStatsOpen();
    } else if (menuSelectedIndex == settingsIdx) {
      onSettingsOpen();
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_READING_STATS), tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, BookFusion, Book, Settings};

  if (hasOpdsUrl) {
    // Insert OPDS Browser after File Browser
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  const int menuOffset = getCoverSlotsUsed();
  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), selectorIndex - menuOffset,
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = contextMenu.isOpen()
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  contextMenu.render(renderer);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onStatsOpen() { activityManager.goToStats(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
