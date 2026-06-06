#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>

#include <algorithm>

#include "../util/ConfirmationActivity.h"
#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
}  // namespace

void FileBrowserActivity::loadFiles() {
  files.clear();
  fileEntries.clear();
  folderEntries.clear();
  fileEntryIsBookFusion.clear();
  authorCache.clear();
  dateAddedCache.clear();
  authorCacheReady = false;
  dateAddedCacheReady = false;

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      continue;
    }

    if (file.isDirectory()) {
      folderEntries.emplace_back(std::string(name) + "/");
    } else {
      std::string_view filename{name};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          fileEntries.emplace_back(filename);
        }
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename)) {
        fileEntries.emplace_back(filename);
      }
    }
  }
  root.close();

  // Folders always keep natural sort and pin to the bottom of the display list.
  FsHelpers::sortFileList(folderEntries);
  authorCache.assign(fileEntries.size(), std::string{});
  dateAddedCache.assign(fileEntries.size(), 0u);

  // BookFusion icon cache — read once at directory load (an expected wait) rather
  // than per-row, per-redraw while scrolling. Only EPUBs can be BF-linked.
  fileEntryIsBookFusion.assign(fileEntries.size(), false);
  {
    std::string baseWithSlash = basepath;
    if (baseWithSlash.empty() || baseWithSlash.back() != '/') baseWithSlash += '/';
    for (size_t i = 0; i < fileEntries.size(); i++) {
      if (FsHelpers::hasEpubExtension(fileEntries[i])) {
        const std::string fullPath = baseWithSlash + fileEntries[i];
        fileEntryIsBookFusion[i] = BookFusionBookIdStore::hasBookId(fullPath.c_str());
      }
    }
  }

  rebuildFilesList();
}

namespace {
std::string_view filenameStemView(const std::string& name) {
  const auto dot = name.find_last_of('.');
  return (dot == std::string::npos) ? std::string_view(name) : std::string_view(name).substr(0, dot);
}
}  // namespace

void FileBrowserActivity::rebuildFilesList() {
  const bool needsAuthor =
      (currentSort == SortMode::AuthorAsc || currentSort == SortMode::AuthorDesc) && !authorCacheReady;
  const bool needsDateAdded =
      (currentSort == SortMode::DateAddedNewest || currentSort == SortMode::DateAddedOldest) && !dateAddedCacheReady;

  std::string baseWithSlash = basepath;
  if (baseWithSlash.empty() || baseWithSlash.back() != '/') baseWithSlash += '/';

  if (needsAuthor) {
    for (size_t i = 0; i < fileEntries.size(); i++) {
      const std::string fullPath = baseWithSlash + fileEntries[i];
      if (FsHelpers::hasEpubExtension(fileEntries[i])) {
        Epub epub(fullPath, "/.crosspoint");
        if (Storage.exists((epub.getCachePath() + "/book.bin").c_str()) && epub.load(true, true)) {
          authorCache[i] = epub.getAuthor();
        }
      } else if (FsHelpers::hasXtcExtension(fileEntries[i])) {
        Xtc xtc(fullPath, "/.crosspoint");
        if (xtc.load()) authorCache[i] = xtc.getAuthor();
      }
      // TXT/MD/BMP have no author metadata; left empty so they sink to the end of Author sorts.
    }
    authorCacheReady = true;
  }

  if (needsDateAdded) {
    for (size_t i = 0; i < fileEntries.size(); i++) {
      const std::string fullPath = baseWithSlash + fileEntries[i];
      HalFile f;
      if (Storage.openFileForRead("FileBrowser", fullPath, f)) {
        dateAddedCache[i] = f.getModifyDateTimePacked();
        f.close();
      }
    }
    dateAddedCacheReady = true;
  }

  const auto& recents = RECENT_BOOKS.getBooks();

  std::vector<SortEntry> entries;
  entries.reserve(fileEntries.size());
  for (size_t i = 0; i < fileEntries.size(); i++) {
    SortEntry e;
    e.sortKey = filenameStemView(fileEntries[i]);
    e.authorKey = authorCacheReady ? std::string_view(authorCache[i]) : std::string_view{};
    e.dateAddedTs = dateAddedCacheReady ? dateAddedCache[i] : 0u;
    e.progressPercent = -1;
    e.lastOpenedRank = LAST_OPENED_NEVER;
    const std::string fullPath = baseWithSlash + fileEntries[i];
    for (size_t r = 0; r < recents.size(); r++) {
      if (recents[r].path == fullPath) {
        e.progressPercent = recents[r].progressPercent;
        e.lastOpenedRank = static_cast<uint16_t>(r);
        break;
      }
    }
    entries.push_back(e);
  }

  std::vector<uint16_t> sortedIndices;
  applySort(sortedIndices, entries, currentSort);

  files.clear();
  filesIsBookFusion.clear();
  files.reserve(fileEntries.size() + folderEntries.size());
  filesIsBookFusion.reserve(fileEntries.size() + folderEntries.size());
  for (uint16_t idx : sortedIndices) {
    files.push_back(fileEntries[idx]);
    filesIsBookFusion.push_back(fileEntryIsBookFusion[idx]);
  }
  for (const auto& folder : folderEntries) {
    files.push_back(folder);
    filesIsBookFusion.push_back(false);
  }
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = 0;
  // Pull last-chosen sort from settings (shared across all list activities).
  currentSort =
      (SETTINGS.sortMode < SORT_MODE_COUNT) ? static_cast<SortMode>(SETTINGS.sortMode) : SortMode::AlphabeticAsc;

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  } else {
    loadFiles();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  // Only clear cache for .epub files
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

void FileBrowserActivity::loop() {
  // Power short-press → sort menu. Suppressed during book-options modal and while
  // a sort rebuild is pending.
  if (!showingBookOptions && !pendingSortRebuild && sortMenu.checkTrigger(mappedInput, currentSort)) {
    requestUpdate();
    return;
  }
  if (sortMenu.isOpen()) {
    SortMode picked;
    bool cancelled = false;
    if (sortMenu.handleInput(buttonNavigator, mappedInput, &picked, &cancelled)) {
      if (!cancelled && picked != currentSort) {
        currentSort = picked;
        SETTINGS.sortMode = static_cast<uint8_t>(picked);
        SETTINGS.saveToFile();
        pendingSortRebuild = true;
        selectorIndex = 0;
      }
      requestUpdate();
    } else {
      requestUpdate();
    }
    return;
  }

  // Note: long-press Back (1s+) is handled globally in main.cpp and returns to Home
  // regardless of the current activity. No per-activity "go to root" gesture anymore.

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  // Long press on a book file → context menu
  if (!showingBookOptions && !longPressBookTriggered && !files.empty() && selectorIndex < files.size()) {
    const std::string& entry = files[selectorIndex];
    const bool isBook =
        entry.back() != '/' && (FsHelpers::hasEpubExtension(entry) || FsHelpers::hasXtcExtension(entry));
    if (isBook && mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 700UL) {
      longPressBookTriggered = true;
      showingBookOptions = true;
      awaitingBookOptionsRelease = true;
      bookOptionsIndex = 0;
      std::string cleanBase = basepath;
      if (cleanBase.back() != '/') cleanBase += '/';
      bookOptionsPath = cleanBase + entry;
      const auto dotPos = entry.rfind('.');
      bookOptionsTitle = (dotPos != std::string::npos) ? entry.substr(0, dotPos) : std::string(entry);
      const auto& recentList = RECENT_BOOKS.getBooks();
      auto it = std::find_if(recentList.begin(), recentList.end(),
                             [this](const RecentBook& rb) { return rb.path == bookOptionsPath; });
      if (it != recentList.end()) {
        bookOptionsAuthor = it->author;
        bookOptionsProgress = it->progressPercent;
      } else {
        bookOptionsAuthor.clear();
        bookOptionsProgress = -1;
      }
      requestUpdate();
      return;
    }
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
        loadFiles();
        if (selectorIndex >= files.size()) selectorIndex = files.empty() ? 0 : files.size() - 1;
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

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    // Firmware picker: selecting a file returns its path to the caller; directories navigate normally.
    if (mode == Mode::PickFirmware && !isDirectory) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      ActivityResult res{FilePathResult{cleanBasePath + entry}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }

    if (mode == Mode::Books && mappedInput.getHeldTime() >= GO_HOME_MS) {
      // --- LONG PRESS ACTION: DELETE FILE OR FOLDER ---
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      // For directories, strip the trailing '/' to get the real path
      const std::string fullPath = cleanBasePath + (isDirectory ? entry.substr(0, entry.length() - 1) : entry);

      auto handler = [this, fullPath, isDirectory](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          bool deleted = isDirectory ? Storage.removeDir(fullPath.c_str()) : Storage.remove(fullPath.c_str());
          if (!isDirectory) clearFileMetadata(fullPath);
          if (deleted) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            loadFiles();
            if (files.empty()) {
              selectorIndex = 0;
            } else if (selectorIndex >= files.size()) {
              selectorIndex = files.size() - 1;
            }
            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
          }
        } else {
          LOG_DBG("FileBrowser", "Delete cancelled by user");
        }
      };

      std::string heading = tr(STR_DELETE) + std::string("? ");

      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else {
        onSelectBook(basepath + entry);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker at root: cancel back to the caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
  }

  // Process normal navigation
  int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextPress([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousPress([this, listSize] {
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

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(std::string filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  // Deferred sort rebuild: show a "Sorting…" popup before doing the (potentially slow)
  // metadata pass on first selection of an Author/DateAdded mode.
  if (pendingSortRebuild) {
    GUI.drawPopup(renderer, tr(STR_SORTING));
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    if (SETTINGS.darkMode) renderer.invertScreen();
    rebuildFilesList();
    pendingSortRebuild = false;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);

  // Show folder name in header; right side shows the active sort label.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str(),
                 sortModeLabel(currentSort));

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) {
          // BookFusion-linked books display the BF mark in the row's icon slot
          // instead of the file-type icon. Sidecar existence is cached at
          // loadFiles() to avoid an SD stat per row, per redraw.
          if (filesIsBookFusion[index]) return UIIcon::BookFusion;
          return UITheme::getFileIcon(files[index]);
        },
        nullptr, false);
  }

  // Full path display
  {
    const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);
    const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);
  }

  // Help text
  // In PickFirmware mode Confirm returns the chosen .bin (not "open"), and Back at root cancels
  // rather than going Home — reflect both in the hints.
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && files[selectorIndex].back() != '/';
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const auto labels = showingBookOptions
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                                  files.empty() ? "" : tr(STR_DIR_DOWN));
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

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
