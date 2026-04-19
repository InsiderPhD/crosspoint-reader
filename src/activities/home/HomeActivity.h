#pragma once
#include <functional>
#include <vector>

#include "../Activity.h"
#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsUrl = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  bool longPressBookTriggered = false;
  bool showingBookOptions = false;
  bool awaitingBookOptionsRelease = false;
  int bookOptionsIndex = 0;
  static constexpr int BOOK_OPTIONS_COUNT = 5;
  static constexpr int BOOK_OPT_MARK_READ = 0;
  static constexpr int BOOK_OPT_RESET_PROGRESS = 1;
  static constexpr int BOOK_OPT_SHELVE = 2;
  static constexpr int BOOK_OPT_DELETE = 3;
  static constexpr int BOOK_OPT_REINDEX = 4;
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  std::vector<RecentBook> recentBooks;
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onStatsOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Home", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
