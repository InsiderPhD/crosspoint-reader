#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/Activity.h"

struct BookFusionBook;

// Bulk action: for every locally-downloaded BookFusion book (those with a
// bookfusion_<hash>.json sidecar), re-download the metadata that comes from the
// BookFusion API rather than the EPUB itself — cover images, organisational
// metadata (categories / bookshelves / lists), and reading position.
//
// The API has no "get book by id" endpoint, so the only source of cover URL +
// categories/shelves/lists is the paginated library search. This walks those
// pages (ALL_BOOKS) and matches each returned book against the local set by
// book_id, stopping early once every local book has been refreshed.
//
// Needs WiFi: modelled on RecacheMetadataActivity (WARNING confirm gate → blocking
// RUNNING pass → SUCCESS result) with the WifiSelectionActivity hand-off used by
// BookFusionBrowserActivity.
class RefreshBookFusionMetadataActivity final : public Activity {
 public:
  explicit RefreshBookFusionMetadataActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RefreshBookFusionMetadata", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }     // Prevent power-saving mode during the pass.
  bool preventAutoSleep() override { return true; }  // WiFi + long walk — don't sleep mid-refresh.
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, WIFI_SELECTION, RUNNING, SUCCESS, ERROR };

  State state = WARNING;

  // Result counters (summarised in SUCCESS).
  int totalLocal = 0;   // Local books with a BookFusion sidecar.
  int matched = 0;      // Local books also found in the remote library walk.
  int coversOk = 0;     // Covers re-cached successfully.
  int metaOk = 0;       // bf_meta.bin sidecars written.
  int positionsOk = 0;  // Reading positions applied (only for books w/o local progress).
  int failed = 0;       // Books where at least one refresh step failed.

  char errorMsg[128] = {};

  void startRun();
  void onWifiComplete(bool success);
  void refreshAll();
  void refreshOneBook(const BookFusionBook& book, const std::string& path, int coverHeight);
  void goBack() { finish(); }
};
