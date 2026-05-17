#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "components/SortMenu.h"
#include "sorting/SortMode.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 private:
  // Deletion
  void clearFileMetadata(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;

  // Book context menu
  bool showingBookOptions = false;
  bool longPressBookTriggered = false;
  bool awaitingBookOptionsRelease = false;
  int bookOptionsIndex = 0;
  std::string bookOptionsPath;
  std::string bookOptionsTitle;
  std::string bookOptionsAuthor;
  int bookOptionsProgress = -1;

  // Files state
  std::string basepath = "/";
  // Display list materialised by rebuildFilesList(): [sorted file entries..., folder entries...].
  // Folder entries are pinned to the bottom in alphabetic order regardless of sort mode.
  std::vector<std::string> files;
  // Raw split — owned source for the display list. fileEntries are sorted per `currentSort`;
  // folderEntries stay alphabetic.
  std::vector<std::string> fileEntries;
  std::vector<std::string> folderEntries;

  // Sort state
  SortMenu sortMenu;
  SortMode currentSort = SortMode::AlphabeticAsc;
  // Lazy metadata caches, parallel-indexed with fileEntries. Cleared whenever loadFiles
  // runs (directory navigation = new directory's data).
  std::vector<std::string> authorCache;
  std::vector<uint32_t> dateAddedCache;
  bool authorCacheReady = false;
  bool dateAddedCacheReady = false;
  bool pendingSortRebuild = false;

  // Data loading
  void loadFiles();
  // Re-materialise `files` from fileEntries + folderEntries using `currentSort`.
  // Populates the lazy caches on first selection of Author* / DateAdded* modes.
  void rebuildFilesList();
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("FileBrowser", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
