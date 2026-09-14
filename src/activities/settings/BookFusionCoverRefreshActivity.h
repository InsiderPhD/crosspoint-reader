#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "BookFusionSyncClient.h"
#include "activities/Activity.h"

// Single-book "Regenerate Cover" for a BookFusion book. BookFusion EPUBs carry
// unreliable embedded covers, so instead of re-extracting from the EPUB this
// re-downloads the API cover at BookFusionCoverCache's fetch size and rebuilds
// every thumbnail height plus the sleep-screen covers from it.
//
// The cover URL isn't stored locally and the API has no get-by-id endpoint, so
// the book is located via the library search: first a title query (usually one
// request), then a full library walk matched by book_id if the query misses.
//
// Flow mirrors RefreshBookFusionMetadataActivity: WifiSelectionActivity hand-off
// when offline → blocking RUNNING pass → SUCCESS/ERROR screen closed with Back.
class BookFusionCoverRefreshActivity final : public Activity {
 public:
  BookFusionCoverRefreshActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                 std::string title)
      : Activity("BookFusionCoverRefresh", renderer, mappedInput), path(std::move(path)), title(std::move(title)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return true; }  // WiFi + blocking fetch — don't sleep mid-run.
  void render(RenderLock&&) override;

 private:
  enum State { STARTING, WIFI_SELECTION, RUNNING, SUCCESS, ERROR };

  State state = STARTING;
  std::string path;
  std::string title;
  char errorMsg[128] = {};
  // Sized like BookFusionBook::coverUrl. A member rather than a local: 384B is
  // over the stack budget, and the activity is already heap-allocated.
  char coverUrl[sizeof(BookFusionBook::coverUrl)] = {};
  // One page of the library search (~8KB). A member, as in
  // BookFusionBrowserActivity: this runs on the 8KB Arduino loop task stack,
  // where a local this size overflows into the heap.
  BookFusionSearchResult searchResult;

  void begin();
  void onWifiComplete(bool success);
  void run();
  void fail(const char* message);
  // Finds the book's API cover URL. Returns false with errorMsg set on failure.
  bool findCoverUrl(uint32_t bookId, char* outUrl, size_t outUrlLen);
};
