#pragma once
#include <OpdsParser.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OpdsBookBrowser", renderer, mappedInput), buttonNavigator() {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Only the BROWSING entry list has hit-testable rows; every other state (and
  // the empty feed) keeps the injected-Confirm tap behaviour.
  bool handlesDirectTouch() const override { return state == BrowserState::BROWSING && !entries.empty(); }

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;  // Added missing member
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;

  // Top edge of the first BROWSING row band; row i spans
  // [listTopY() + i*ROW_H, + ROW_H). Shared by render() and the Full Touch tap
  // hit-testing in loop().
  static constexpr int ROW_H = 30;
  int listTopY() const;
  // The BROWSING Confirm action for the selected entry, also fired by a Full
  // Touch tap on the already-selected row.
  void activateSelectedEntry();

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override { return true; }
};
