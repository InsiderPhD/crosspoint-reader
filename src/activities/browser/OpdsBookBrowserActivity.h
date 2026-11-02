#pragma once
#include <OpdsParser.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "OpdsServerStore.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState {
    SERVER_SELECT,
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    DESCRIPTION,
    DOWNLOADING,
    ERROR,
    SEARCH_INPUT
  };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OpdsBookBrowser", renderer, mappedInput), buttonNavigator() {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Only the BROWSING entry list has hit-testable rows; every other state (and
  // the empty feed) keeps the injected-Confirm tap behaviour.
  bool handlesDirectTouch() const override {
    return state == BrowserState::SERVER_SELECT || (state == BrowserState::BROWSING && !entries.empty());
  }

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
  // The catalog being browsed, copied when it is picked so an edit made from
  // the web UI meanwhile cannot change it under an open feed.
  OpdsServer server;
  // That catalog's login and headers, sent only to its own origin (a redirect
  // to a CDN or another host does not receive them).
  HttpDownloader::ScopedAuth auth;
  // Cursor on the catalog picker (shown only when more than one is saved).
  int serverIndex = 0;
  // DESCRIPTION state: the selected book's excerpt and the first line shown.
  // Held only while the preview is open (freed by closeDescription()).
  static constexpr size_t DESCRIPTION_MAX_BYTES = 1536;
  std::string descriptionText;
  int descriptionTop = 0;
  // Description lines per screen, written by render() for loop()'s scrolling.
  int descriptionPageLines = 1;

  // Top edge of the first BROWSING row band; row i spans
  // [listTopY() + i*ROW_H, + ROW_H). Shared by render() and the Full Touch tap
  // hit-testing in loop().
  static constexpr int ROW_H = 30;
  int listTopY() const;
  // The BROWSING Confirm action for the selected entry, also fired by a Full
  // Touch tap on the already-selected row.
  void activateSelectedEntry();

  // Catalog picker: shown first when several catalogs are saved, and again on
  // Back from a catalog's root feed.
  void showServerSelect();
  void openServer(size_t index);
  Rect serverListRect() const;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  // Book preview before a download: re-reads the current feed for just that
  // book's description (bounded, see DESCRIPTION_MAX_BYTES), then shows it.
  void showDescription(const OpdsEntry& book);
  void closeDescription();
  void launchSearch();
  void performSearch(const std::string& query);
  // Holds the device awake only while something is in flight. An idle catalog
  // list or an error screen sleeps on the normal timeout like any other screen.
  bool preventAutoSleep() override;
};
