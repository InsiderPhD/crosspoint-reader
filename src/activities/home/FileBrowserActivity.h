#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "components/SortMenu.h"
#include "sorting/SortMode.h"
#include "util/ButtonNavigator.h"

struct Rect;

class FileBrowserActivity final : public Activity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return the chosen
  // path to the caller via ActivityResult (FilePathResult) instead of opening it as a book.
  enum class Mode { Books, PickFirmware };

 private:
  // Deletion
  void clearFileMetadata(const std::string& fullPath);

  const Mode mode = Mode::Books;

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;

  // Book context menu
  bool showingBookOptions = false;
  bool longPressBookTriggered = false;
  bool awaitingBookOptionsRelease = false;
  int bookOptionsIndex = 0;
  // Popup layout cached from render()'s drawBookOptionsPopup call so loop()
  // can hit-test taps (Full Touch): outside tap closes, tap on an unselected
  // option row moves the highlight, tap on the highlighted row activates via
  // the injected Confirm. Invalid until the open popup has been drawn once.
  int bookOptionsPopupX = 0;
  int bookOptionsPopupY = 0;
  int bookOptionsPopupW = 0;
  int bookOptionsPopupH = 0;
  int bookOptionsOptionsTop = 0;
  int bookOptionsRowH = 1;
  bool bookOptionsLayoutValid = false;
  std::string bookOptionsPath;
  std::string bookOptionsTitle;
  std::string bookOptionsAuthor;
  int bookOptionsProgress = -1;
  bool bookOptionsHasClippings = false;

  // Files state
  std::string basepath = "/";
  // Display list materialised by rebuildFilesList(): [sorted file entries..., folder entries...].
  // Folder entries are pinned to the bottom in alphabetic order regardless of sort mode.
  std::vector<std::string> files;
  // Raw split — owned source for the display list. fileEntries are sorted per `currentSort`;
  // folderEntries stay alphabetic.
  std::vector<std::string> fileEntries;
  std::vector<std::string> folderEntries;
  // Parallel to fileEntries: true if the file has a BookFusion sidecar.
  // Populated once at loadFiles(); avoids an SD stat per row, per redraw, while scrolling.
  std::vector<bool> fileEntryIsBookFusion;
  // Parallel to `files` (the display list); rebuilt by rebuildFilesList().
  std::vector<bool> filesIsBookFusion;
  // Parallel to `files` (the display list): reading progress percent (-1 = none/unknown),
  // looked up from RECENT_BOOKS during rebuildFilesList(). Folders are always -1.
  std::vector<int8_t> filesProgress;

  // Sort state
  SortMenu sortMenu;
  SortMode currentSort = SortMode::AlphabeticAsc;
  // Lazy metadata caches, parallel-indexed with fileEntries. Cleared whenever loadFiles
  // runs (directory navigation = new directory's data).
  std::vector<std::string> authorCache;
  std::vector<std::string> tagCache;
  std::vector<uint32_t> dateAddedCache;
  bool authorCacheReady = false;
  bool tagCacheReady = false;
  bool dateAddedCacheReady = false;
  bool pendingSortRebuild = false;

  // Data loading
  void loadFiles();
  // Re-materialise `files` from fileEntries + folderEntries using `currentSort`.
  // Populates the lazy caches on first selection of Author* / DateAdded* modes.
  void rebuildFilesList();
  size_t findEntry(const std::string& name) const;

  // List body between the header and the path strip. Shared by render() and
  // the Full Touch tap hit-testing so the two can never disagree.
  Rect listRect() const;
  // Open/navigate the entry at selectorIndex — the Confirm short-press body,
  // also fired by a Full Touch tap on a row.
  void activateSelectedEntry();
  // Open the book context menu for files[selectorIndex]. awaitRelease: the
  // button path enters with Confirm still physically held and must swallow it
  // until release; the touch path suppressed its contact, so nothing is held.
  void openBookOptions(bool awaitRelease);
  // Hold-Confirm delete flow for files[selectorIndex] (confirmation dialog +
  // removal), also fired by a Full Touch long tap on a non-book row.
  void confirmDeleteSelectedEntry();

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books)
      : Activity("FileBrowser", renderer, mappedInput),
        mode(mode),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // While a modal without tap hit-testing is open, hand taps back to the
  // global tap-is-Confirm injection so a tap activates the highlighted option.
  bool handlesDirectTouch() const override { return !showingBookOptions && !sortMenu.isOpen(); }
};
