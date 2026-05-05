#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Xtc.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

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
      files.emplace_back(std::string(name) + "/");
    } else {
      std::string_view filename{name};
      if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
          FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
          FsHelpers::hasBmpExtension(filename)) {
        files.emplace_back(filename);
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = 0;

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

  // Long press BACK (1s+) goes to root folder
  // but Long press BACK (1s+) from ReaderActivity sends us here with the MappedInput already set.
  // So ignore it the first time.
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/" && !lockLongPressBack && !showingBookOptions) {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  // Long press on a book file → context menu
  if (!showingBookOptions && !longPressBookTriggered && !files.empty() && selectorIndex < files.size()) {
    const std::string& entry = files[selectorIndex];
    const bool isBook = entry.back() != '/' &&
                        (FsHelpers::hasEpubExtension(entry) || FsHelpers::hasXtcExtension(entry));
    if (isBook && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= 700UL) {
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

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    if (mappedInput.getHeldTime() >= GO_HOME_MS) {
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
      } else {
        onGoHome();
      }
    }
  }

  // Process normal navigation
    int listSize = static_cast<int>(files.size());
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
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);

  // Show folder name in header
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) {
          std::string filename = getFileName(files[index]);
          // Mark BookFusion-linked books with an "&" prefix — a visual cue that this
          // entry "references" a cloud-side BookFusion book and will sync to it.
          if (files[index].back() != '/') { // Not a directory
            const std::string fullPath = (basepath.back() == '/') ? basepath + files[index] : basepath + "/" + files[index];
            if (BookFusionBookIdStore::loadBookId(fullPath.c_str()) != 0) {
              return "& " + filename;
            }
          }
          return filename;
        }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); },
        [this](int index) -> std::string {
          const std::string& entry = files[index];
          if (entry.back() == '/') return "";  // Directory
          const std::string fullPath = (basepath.back() == '/') ? basepath + entry : basepath + "/" + entry;
          for (const auto& book : RECENT_BOOKS.getBooks()) {
            if (book.path == fullPath && book.progressPercent > 0) {
              if (book.progressPercent >= 90) return "X";
              char buf[6];
              snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(book.progressPercent));
              return std::string(buf);
            }
          }
          return "";
        },
        false);
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
  const auto labels = showingBookOptions
      ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
      : mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK), files.empty() ? "" : tr(STR_OPEN),
                              files.empty() ? "" : tr(STR_DIR_UP), files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showingBookOptions) {
    const auto lastSlash = bookOptionsPath.rfind('/');
    const std::string folder =
        (lastSlash != std::string::npos) ? bookOptionsPath.substr(0, lastSlash) : bookOptionsPath;
    UITheme::drawBookOptionsPopup(renderer, bookOptionsTitle.c_str(), bookOptionsAuthor.c_str(), folder.c_str(),
                                  bookOptionsProgress, bookOptionsIndex);
  }

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
