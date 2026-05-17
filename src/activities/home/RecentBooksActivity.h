#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "components/SortMenu.h"
#include "sorting/SortMode.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Book context menu
  bool showingBookOptions = false;
  bool longPressBookTriggered = false;
  bool awaitingBookOptionsRelease = false;
  int bookOptionsIndex = 0;
  std::string bookOptionsPath;
  std::string bookOptionsTitle;
  std::string bookOptionsAuthor;
  int bookOptionsProgress = -1;

  // Recent tab state
  std::vector<RecentBook> recentBooks;

  // Sort state
  SortMenu sortMenu;
  SortMode currentSort = SortMode::LastOpenedNewest;
  std::vector<uint16_t> sortedIndices;

  // Data loading
  void loadRecentBooks();
  void rebuildSortedIndices();

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
