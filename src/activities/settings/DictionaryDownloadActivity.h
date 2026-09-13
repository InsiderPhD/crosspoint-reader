#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#ifndef DICT_CATALOG_URL
// Catalog of prebuilt StarDict dictionaries (Wiktionary-derived, plus Webster
// 1913). The file is a flat index of every dictionary — the paginated
// all-<n>.json siblings exist for the plugin host's page-at-a-time browser and
// carry no size field, which this screen needs up front.
//
// Overridable from platformio.local.ini so a fork can point at its own mirror
// without touching the source.
#define DICT_CATALOG_URL \
  "https://raw.githubusercontent.com/itsthisjustin/sd-plugins/main/dictionaries/catalog/index.json"
#endif

struct Rect;

/**
 * Downloads prebuilt StarDict dictionaries into /dictionaries/<id>/.
 *
 * Same shape as FontDownloadActivity (WiFi -> catalog -> list -> per-file
 * transfer on a frozen screen), with two differences the file sizes force:
 * these downloads run to 70MB rather than a few hundred KB, so the list shows
 * the size before you commit and the transfer is cancellable mid-flight; and
 * an installed dictionary can be deleted from the same list, because a card
 * filled with 60MB language packs is otherwise only recoverable on a computer.
 */
class DictionaryDownloadActivity final : public Activity {
 public:
  explicit DictionaryDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == LOADING_CATALOG || state_ == DOWNLOADING; }
  bool skipLoopDelay() override { return true; }
  // The catalog list hit-tests taps itself, and the delete confirmation opts
  // out of tap-is-Confirm entirely: a tap anywhere must not be able to delete a
  // 60MB download. Both still get the action bar, which is the deliberate way
  // to answer either screen. COMPLETE/ERROR keep the global injection so a tap
  // dismisses them.
  bool handlesDirectTouch() const override {
    return (state_ == CATALOG_LIST && !entries_.empty()) || state_ == CONFIRM_DELETE || state_ == DOWNLOADING;
  }

  // Public: the streaming catalog parser (.cpp) fills these as the body arrives.
  struct CatalogEntry {
    std::string id;        // folder name under /dictionaries
    std::string title;     // display name ("English (Webster 1913)")
    std::string subtitle;  // catalog's "author" field: "160k entries, 28.9 MB"
    std::string base;      // URL prefix the files hang off
    std::vector<std::string> files;
    size_t bytes = 0;  // total install size, 0 when the catalog omits it
    bool installed = false;
  };

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_CATALOG,
    CATALOG_LIST,
    CONFIRM_DELETE,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  State state_ = WIFI_SELECTION;
  ButtonNavigator buttonNavigator_;

  std::vector<CatalogEntry> entries_;
  int selectedIndex_ = 0;

  // Download progress. The screen is frozen for the duration of each file (the
  // framebuffer is lent to the TLS session), so this is a between-files
  // counter, not a live bar.
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  int downloadingIndex_ = -1;
  std::string statusDetail_;
  std::string errorMessage_;

  // Set from inside the transfer's progress callback when the user presses
  // Back, read by HttpDownloader once per chunk. Single task, no ISR.
  volatile bool cancelRequested_ = false;
  unsigned long lastCancelPollMs_ = 0;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseCatalog();
  void refreshInstalledFlags();
  void downloadEntry(CatalogEntry& entry);
  void deleteSelected();
  void activateSelected();
  // Polls input mid-transfer and latches cancelRequested_ on Back. Called from
  // the download progress callback, which runs with the render lock held — it
  // must never render or request an update.
  void pollForCancel();

  Rect listRect() const;
  int listItemCount() const { return static_cast<int>(entries_.size()); }
  static std::string formatSize(size_t bytes);
};
