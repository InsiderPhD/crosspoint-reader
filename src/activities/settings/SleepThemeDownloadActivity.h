#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#ifndef SLEEP_THEME_CATALOG_URL
// The sleep-theme store: themes compiled for every panel size by the store's
// CI. Each theme's files hang off <catalog dir>/<build key>/<id>/.
//
// Overridable from platformio.local.ini so a fork can point at its own store.
#define SLEEP_THEME_CATALOG_URL \
  "https://raw.githubusercontent.com/InsiderPhD/katiepoint-sleep-themes/main/dist/index.json"
#endif

struct Rect;

/**
 * Downloads compiled sleep themes from the theme store into /sleep-themes/<id>/.
 *
 * Same shape as DictionaryDownloadActivity (WiFi -> catalog -> list -> per-file
 * transfer on a frozen screen, cancellable, Confirm on an installed row
 * deletes it). Themes are compiled per panel size, so only the build for this
 * device's screen is fetched, and a theme without one is not listed.
 *
 * Preview: Confirm on a theme that would download (new or out of date) first
 * shows the store's preview.png for this device's build (a screenshot the
 * store's CI renders beside the build; not one of the theme's files), with
 * Back / Download. The PNG lands on the SD card, is converted once to a BMP
 * fitted to the preview area, and is deleted again when the preview closes.
 * A missing or undecodable preview says so and still offers the download.
 * Every install also saves the preview, fitted within the panel's long side
 * squared, as <theme>/preview.bmp (SleepThemeRenderer::PREVIEW_FILE) for
 * SleepThemeSelectActivity; one that fails to fetch or convert is skipped.
 *
 * Updates: each build has a version (a hash of its files) that an install
 * records in the theme's store.ver. An installed theme whose version differs
 * (or that has none: installed by hand or by older firmware) is listed as
 * Update, Confirm reinstalls it, and an "Update All" row at the top
 * reinstalls every one of them.
 *
 * A theme downloads into a hidden /sleep-themes/.<id>.part/ folder and is
 * swapped into place only once every file has arrived, so neither the picker
 * nor the sleep screen ever sees half a theme.
 */
class SleepThemeDownloadActivity final : public Activity {
 public:
  explicit SleepThemeDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_CATALOG || state_ == DOWNLOADING || state_ == PREVIEW_LOADING;
  }
  bool skipLoopDelay() override { return true; }
  // As in DictionaryDownloadActivity: the list hit-tests taps itself, and the
  // delete confirmation must not be answerable by a stray tap.
  bool handlesDirectTouch() const override {
    return (state_ == CATALOG_LIST && !entries_.empty()) || state_ == CONFIRM_DELETE || state_ == DOWNLOADING ||
           state_ == PREVIEW_LOADING || state_ == PREVIEW;
  }
  // The preview's Download is only reachable from the action bar (a tap on the
  // picture does nothing), so its Confirm slot must stay.
  bool tapActivatesConfirm() const override {
    return handlesDirectTouch() && state_ != PREVIEW_LOADING && state_ != PREVIEW;
  }

  // Public: the streaming catalog parser (.cpp) fills these as the body arrives.
  struct CatalogFile {
    std::string name;  // theme.bin, bg.bmp, mask.bmp, front.bmp or f0..f3.cpfont
    uint32_t size = 0;
  };
  struct CatalogEntry {
    std::string id;                  // folder name under /sleep-themes
    std::string name;                // display name
    std::string description;         // one line
    std::vector<CatalogFile> files;  // this device's build only
    size_t bytes = 0;                // total of files[].size
    std::string version;             // this build's version; empty in an older catalog
    bool installed = false;
    bool updateAvailable = false;  // installed, and its store.ver differs from version
  };

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_CATALOG,
    CATALOG_LIST,
    CONFIRM_DELETE,
    PREVIEW_LOADING,  // fetching + converting the selected theme's preview.png
    PREVIEW,          // preview on screen: Back / Download
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  State state_ = WIFI_SELECTION;
  ButtonNavigator buttonNavigator_;

  std::vector<CatalogEntry> entries_;
  int selectedIndex_ = 0;

  // Between-files progress: the screen is frozen during each transfer (the
  // framebuffer is lent to the TLS session).
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  int downloadingIndex_ = -1;
  std::string errorMessage_;
  // PREVIEW: whether the fitted preview BMP is on the card (false = show the
  // "no preview" line instead).
  bool previewReady_ = false;
  // The theme whose preview PNG is waiting at the card root (empty = none).
  std::string previewId_;

  // Latched from the transfer's progress callback on Back; read by
  // HttpDownloader once per chunk. Single task, no ISR.
  volatile bool cancelRequested_ = false;
  unsigned long lastCancelPollMs_ = 0;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseCatalog();
  void refreshInstalledFlags();
  // False when it stopped on an error or a cancel (state_ says which).
  bool downloadEntry(CatalogEntry& entry);
  void updateAll();
  void deleteSelected();
  void activateSelected();
  // Fetches + converts the selected entry's preview, then shows PREVIEW (or
  // returns to the list if Back cancelled the fetch).
  void openPreview(const CatalogEntry& entry);
  bool fetchPreview(const CatalogEntry& entry);
  // Removes the preview's scratch files.
  void discardPreview();
  // The fitted preview picture's box: the list body, inset.
  Rect previewRect() const;
  // Polls input mid-transfer and latches cancelRequested_ on Back. Runs with
  // the render lock held: it must never render or request an update.
  void pollForCancel();

  Rect listRect() const;
  // Rows: "Update All" first while any installed theme is out of date, then
  // one per catalog entry.
  int updateCount() const;
  int entryRowOffset() const { return updateCount() > 0 ? 1 : 0; }
  int listItemCount() const { return static_cast<int>(entries_.size()) + entryRowOffset(); }
  bool isUpdateAllRow(const int row) const { return entryRowOffset() == 1 && row == 0; }
  // The entry a row shows, or nullptr for the Update All row.
  CatalogEntry* entryAtRow(int row);
  static std::string formatSize(size_t bytes);
};
