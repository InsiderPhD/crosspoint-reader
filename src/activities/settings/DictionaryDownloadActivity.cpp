#include "DictionaryDownloadActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <StreamingJsonParser.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/DictionaryRegistry.h"
#include "util/TouchListNav.h"

namespace {

// Scratch file the catalog is downloaded to before it is parsed. Same two-phase
// approach as the font manifest: streaming the parse during the live TLS
// session runs the heap dry building the entry strings.
constexpr const char* CATALOG_TMP = "/dict_catalog.tmp";

// Guard against a catalog that grows without bound: 21 dictionaries is ~6KB
// today, and every entry costs heap for the duration of the screen.
constexpr size_t MAX_CATALOG_ENTRIES = 64;

// Streaming catalog parser (same SAX pattern as FontDownloadActivity's manifest
// stream). Entries land directly in the output vector — no DOM, which for this
// document would cost 2-3x the source on a heap that has ~28KB to spare.
//
// Catalog shape:
//   { "generated": "...", "items": [
//       { "id": "...", "title": "...", "author": "...", "base": "https://.../",
//         "files": ["x.ifo", "x.idx", "x.dict.dz"], "bytes": N } ] }
class DictCatalogJsonStream final : public Stream {
 public:
  explicit DictCatalogJsonStream(std::vector<DictionaryDownloadActivity::CatalogEntry>& outEntries)
      : outEntries_(outEntries),
        parser_(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                              sOnArrayStart, sOnArrayEnd}) {
    outEntries_.clear();
  }

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    bytesRead_ += size;
    parser_.feed(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t bytesRead() const { return bytesRead_; }
  bool ok() const { return bytesRead_ > 0 && rootSeen_ && rootClosed_ && depth_ == 0 && !parser_.hasError(); }

 private:
  enum class LastKey : uint8_t {
    NONE,
    ITEMS,
    ID,
    TITLE,
    AUTHOR,
    BASE,
    FILES,
    BYTES,
  };

  static void sOnKey(void* ctx, const char* key, size_t len) {
    static_cast<DictCatalogJsonStream*>(ctx)->onKey(key, len);
  }
  static void sOnString(void* ctx, const char* value, size_t len) {
    static_cast<DictCatalogJsonStream*>(ctx)->onString(value, len);
  }
  static void sOnNumber(void* ctx, const char* value, size_t len) {
    static_cast<DictCatalogJsonStream*>(ctx)->onNumber(value, len);
  }
  static void sOnBool(void* ctx, bool /*value*/) { static_cast<DictCatalogJsonStream*>(ctx)->lastKey_ = LastKey::NONE; }
  static void sOnNull(void* ctx) { static_cast<DictCatalogJsonStream*>(ctx)->lastKey_ = LastKey::NONE; }
  static void sOnObjectStart(void* ctx) { static_cast<DictCatalogJsonStream*>(ctx)->onObjectStart(); }
  static void sOnObjectEnd(void* ctx) { static_cast<DictCatalogJsonStream*>(ctx)->onObjectEnd(); }
  static void sOnArrayStart(void* ctx) { static_cast<DictCatalogJsonStream*>(ctx)->onArrayStart(); }
  static void sOnArrayEnd(void* ctx) { static_cast<DictCatalogJsonStream*>(ctx)->onArrayEnd(); }

  static bool keyIs(const char* key, size_t len, const char* expected) {
    return strlen(expected) == len && memcmp(key, expected, len) == 0;
  }

  void onKey(const char* key, size_t len) {
    lastKey_ = LastKey::NONE;
    if (inItem_ && depth_ == itemDepth_) {
      if (keyIs(key, len, "id"))
        lastKey_ = LastKey::ID;
      else if (keyIs(key, len, "title"))
        lastKey_ = LastKey::TITLE;
      else if (keyIs(key, len, "author"))
        lastKey_ = LastKey::AUTHOR;
      else if (keyIs(key, len, "base"))
        lastKey_ = LastKey::BASE;
      else if (keyIs(key, len, "files"))
        lastKey_ = LastKey::FILES;
      else if (keyIs(key, len, "bytes"))
        lastKey_ = LastKey::BYTES;
    } else if (rootSeen_ && depth_ == 1) {
      if (keyIs(key, len, "items")) lastKey_ = LastKey::ITEMS;
    }
  }

  void onString(const char* value, size_t len) {
    switch (lastKey_) {
      case LastKey::ID:
        current_.id.assign(value, len);
        break;
      case LastKey::TITLE:
        current_.title.assign(value, len);
        break;
      case LastKey::AUTHOR:
        current_.subtitle.assign(value, len);
        break;
      case LastKey::BASE:
        current_.base.assign(value, len);
        break;
      default:
        if (inFiles_ && depth_ == filesDepth_ && current_.files.size() < 8) {
          current_.files.emplace_back(value, len);
        }
        break;
    }
    lastKey_ = LastKey::NONE;
  }

  void onNumber(const char* value, size_t /*len*/) {
    // Parser tokens are null-terminated (the contract every other stream here
    // relies on), so strtoul is safe on the raw pointer.
    if (lastKey_ == LastKey::BYTES) {
      current_.bytes = static_cast<size_t>(strtoul(value, nullptr, 10));
    }
    lastKey_ = LastKey::NONE;
  }

  void onObjectStart() {
    if (depth_ == 0) {
      rootSeen_ = true;
    } else if (inItems_ && !inItem_ && depth_ == itemsDepth_) {
      current_ = DictionaryDownloadActivity::CatalogEntry{};
      current_.files.reserve(4);
      itemDepth_ = depth_ + 1;
      inItem_ = true;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onObjectEnd() {
    if (inItem_ && depth_ == itemDepth_) {
      // Drop entries the installer could not act on rather than surfacing a row
      // that fails the moment it is pressed. The id and every filename are
      // concatenated into SD paths, so both are validated here, once.
      const bool usable = !current_.id.empty() && !current_.base.empty() && !current_.files.empty() &&
                          DictionaryRegistry::isValidName(current_.id.c_str());
      bool filesOk = usable;
      if (usable) {
        for (const auto& f : current_.files) {
          if (!DictionaryRegistry::isValidFileName(f.c_str())) {
            filesOk = false;
            break;
          }
        }
      }
      if (filesOk && outEntries_.size() < MAX_CATALOG_ENTRIES) {
        if (current_.title.empty()) current_.title = current_.id;
        outEntries_.push_back(std::move(current_));
      } else if (!filesOk) {
        LOG_DBG("DDL", "Skipping unusable catalog entry: %s", current_.id.c_str());
      }
      inItem_ = false;
      itemDepth_ = 0;
    }
    if (depth_ > 0) --depth_;
    if (rootSeen_ && depth_ == 0) rootClosed_ = true;
    lastKey_ = LastKey::NONE;
  }

  void onArrayStart() {
    if (lastKey_ == LastKey::ITEMS && depth_ == 1) {
      inItems_ = true;
      itemsDepth_ = depth_ + 1;
    } else if (inItem_ && lastKey_ == LastKey::FILES && depth_ == itemDepth_) {
      inFiles_ = true;
      filesDepth_ = depth_ + 1;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onArrayEnd() {
    if (inFiles_ && depth_ == filesDepth_) {
      inFiles_ = false;
      filesDepth_ = 0;
    } else if (inItems_ && depth_ == itemsDepth_) {
      inItems_ = false;
      itemsDepth_ = 0;
    }
    if (depth_ > 0) --depth_;
    lastKey_ = LastKey::NONE;
  }

  std::vector<DictionaryDownloadActivity::CatalogEntry>& outEntries_;
  StreamingJsonParser parser_;
  DictionaryDownloadActivity::CatalogEntry current_;
  LastKey lastKey_ = LastKey::NONE;
  size_t bytesRead_ = 0;
  uint8_t depth_ = 0;
  uint8_t itemsDepth_ = 0;
  uint8_t itemDepth_ = 0;
  uint8_t filesDepth_ = 0;
  bool rootSeen_ = false;
  bool rootClosed_ = false;
  bool inItems_ = false;
  bool inItem_ = false;
  bool inFiles_ = false;
};

}  // namespace

DictionaryDownloadActivity::DictionaryDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("DictionaryDownload", renderer, mappedInput) {}

// --- Lifecycle ---

void DictionaryDownloadActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("DDL", "Heap at entry: free=%u largest=%u", ESP.getFreeHeap(),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void DictionaryDownloadActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void DictionaryDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_CATALOG;
  }
  requestUpdateAndWait();

  if (!fetchAndParseCatalog()) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }

  RenderLock lock(*this);
  state_ = CATALOG_LIST;
  selectedIndex_ = 0;
}

// --- Catalog fetching ---

bool DictionaryDownloadActivity::fetchAndParseCatalog() {
  // Shed the cached glyphs before entering TLS: this screen reaches the
  // handshake with the same tight heap the font downloader does, and the
  // caches re-warm on the next paint.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCachesDeep();
  }
  LOG_DBG("DDL", "Heap after cache shed: free=%u largest=%u", ESP.getFreeHeap(),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));

  // Two SERIAL phases so the heap never carries both costs at once: (1) fetch
  // the raw catalog to SD inside the framebuffer borrow, (2) stream-parse the
  // file on a quiet heap once the connection is closed.
  auto dlResult = HttpDownloader::HTTP_ERROR;
  {
    TlsFramebufferBorrow borrow(renderer);
    dlResult = HttpDownloader::downloadToFile(DICT_CATALOG_URL, CATALOG_TMP, nullptr);
  }
  if (dlResult != HttpDownloader::OK) {
    LOG_ERR("DDL", "Failed to fetch catalog from %s", DICT_CATALOG_URL);
    errorMessage_ = tr(STR_DICT_LIST_FAILED);
    Storage.remove(CATALOG_TMP);
    return false;
  }

  FsFile catalogFile;
  if (!Storage.openFileForRead("DDL", CATALOG_TMP, catalogFile)) {
    LOG_ERR("DDL", "Failed to open temp catalog");
    Storage.remove(CATALOG_TMP);
    errorMessage_ = tr(STR_DICT_LIST_FAILED);
    return false;
  }

  DictCatalogJsonStream stream(entries_);
  entries_.reserve(24);
  uint8_t chunk[256];
  int got;
  while ((got = catalogFile.read(chunk, sizeof(chunk))) > 0) {
    stream.write(chunk, static_cast<size_t>(got));
  }
  catalogFile.close();
  Storage.remove(CATALOG_TMP);

  if (!stream.ok()) {
    LOG_ERR("DDL", "Catalog streaming parse error after %zu bytes", stream.bytesRead());
    errorMessage_ = tr(STR_DICT_LIST_INVALID);
    entries_.clear();
    return false;
  }

  refreshInstalledFlags();
  LOG_DBG("DDL", "Catalog loaded: %zu dictionaries", entries_.size());
  return true;
}

// Mark catalog rows that are already on the card. Runs off a single directory
// scan rather than a per-row Storage.exists(), and doubles as the post-download
// verification: an install only counts once DictionaryRegistry can actually
// resolve the folder to an .idx plus a .dict/.dict.dz.
void DictionaryDownloadActivity::refreshInstalledFlags() {
  std::vector<DictionaryEntry> installed;
  DictionaryRegistry::discover(installed);

  for (auto& entry : entries_) {
    entry.installed = false;
    for (const auto& found : installed) {
      if (found.name == entry.id) {
        entry.installed = true;
        break;
      }
    }
  }
}

// --- Download ---

void DictionaryDownloadActivity::pollForCancel() {
  // Called from inside the transfer with the render lock held: poll input, set
  // a flag, and nothing else. Throttled because each call reaches the GT911
  // over I2C on the X4 Pro, and the transport delivers chunks at a far higher
  // rate than a human presses a button.
  const unsigned long now = millis();
  if (now - lastCancelPollMs_ < 250) return;
  lastCancelPollMs_ = now;

  mappedInput.update();
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    LOG_INF("DDL", "Cancel requested");
    cancelRequested_ = true;
  }
}

void DictionaryDownloadActivity::downloadEntry(CatalogEntry& entry) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingIndex_ = static_cast<int>(&entry - entries_.data());
    currentFileIndex_ = 0;
    currentFileTotal_ = entry.files.size();
    statusDetail_.clear();
  }
  cancelRequested_ = false;
  lastCancelPollMs_ = millis();

  std::string destDir;
  if (!DictionaryRegistry::ensureInstallDir(entry.id.c_str(), destDir)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_DICT_INSTALL_DIR_FAILED);
    return;
  }

  // Keep the radio awake for the whole dictionary: WiFi power-save on an idle
  // link (the user has just been sitting on the list) surfaces as a TCP connect
  // failure on the first request. Restored on every return path.
  esp_wifi_set_ps(WIFI_PS_NONE);
  struct PsRestore {
    ~PsRestore() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
  } psRestore;

  for (size_t i = 0; i < entry.files.size(); i++) {
    const auto& file = entry.files[i];

    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
    }
    // Paint the per-file frame and wait for it: the screen then freezes on it
    // while the framebuffer is lent to this file's TLS session. The (i/n)
    // counter is the only progress the user gets, so it must be on the panel
    // before the transfer starts.
    requestUpdateAndWait();

    // Browsing the list re-warms the glyph cache after the pre-catalog clear,
    // and these transfers are the longest in the firmware — shed again so the
    // whole transfer runs on the roomiest heap available.
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCachesDeep();
    }
    LOG_DBG("DDL", "Heap before %s: free=%u largest=%u", file.c_str(), ESP.getFreeHeap(),
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));

    const std::string url = entry.base + file;
    const std::string destPath = destDir + "/" + file;

    // Retried like every other transfer here: a dropped CDN connection part-way
    // through a 60MB file must not throw away the files already fetched.
    auto result = HttpDownloader::HTTP_ERROR;
    constexpr int kMaxFileAttempts = 3;
    for (int attempt = 1; attempt <= kMaxFileAttempts; attempt++) {
      TlsFramebufferBorrow borrow(renderer);
      size_t lastLoggedMB = 0;
      result = HttpDownloader::downloadToFile(
          url, destPath,
          [this, &lastLoggedMB](size_t downloaded, size_t total) {
            pollForCancel();
            const size_t mb = downloaded >> 20;
            if (mb > lastLoggedMB) {
              lastLoggedMB = mb;
              LOG_DBG("DDL", "Download progress: %u MB", (unsigned)mb);
              (void)total;
            }
          },
          /*allowConfiguredAuth=*/false, /*expectedSize=*/0, &cancelRequested_);
      if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED) break;
      LOG_ERR("DDL", "Download attempt %d/%d failed: %s (%d)", attempt, kMaxFileAttempts, file.c_str(), result);
    }

    if (result == HttpDownloader::ABORTED) {
      // A cancelled install leaves a half-written folder that discover() would
      // either reject or, worse, accept with a truncated index. Remove it.
      DictionaryRegistry::removeDictionary(entry.id.c_str());
      refreshInstalledFlags();
      RenderLock lock(*this);
      state_ = CATALOG_LIST;
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("DDL", "Download failed: %s (%d)", file.c_str(), result);
      DictionaryRegistry::removeDictionary(entry.id.c_str());
      refreshInstalledFlags();
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string(tr(STR_DOWNLOAD_FAILED)) + ": " + file;
      return;
    }
  }

  // Verification: the folder only counts as installed once the registry can
  // resolve it, which is exactly the check the picker and the reader make.
  refreshInstalledFlags();
  if (!entry.installed) {
    LOG_ERR("DDL", "Installed files did not resolve to a dictionary: %s", entry.id.c_str());
    DictionaryRegistry::removeDictionary(entry.id.c_str());
    refreshInstalledFlags();
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_DICT_INSTALL_INVALID);
    return;
  }

  // First dictionary on the device becomes the active one: the alternative is
  // a user who downloads a dictionary, returns to the reader and still gets
  // "No dictionary set".
  if (SETTINGS.dictionaryName[0] == '\0') {
    strncpy(SETTINGS.dictionaryName, entry.id.c_str(), sizeof(SETTINGS.dictionaryName) - 1);
    SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
    SETTINGS.saveToFile();
    LOG_INF("DDL", "Auto-selected first dictionary: %s", entry.id.c_str());
  }

  RenderLock lock(*this);
  state_ = COMPLETE;
}

void DictionaryDownloadActivity::deleteSelected() {
  if (selectedIndex_ < 0 || selectedIndex_ >= listItemCount()) return;
  auto& entry = entries_[static_cast<size_t>(selectedIndex_)];

  const bool wasActive = strncmp(SETTINGS.dictionaryName, entry.id.c_str(), sizeof(SETTINGS.dictionaryName) - 1) == 0;
  const bool removed = DictionaryRegistry::removeDictionary(entry.id.c_str());
  refreshInstalledFlags();

  // Clear the setting rather than leave it pointing at a folder that is gone:
  // the reader would refuse every lookup with a message about a dictionary the
  // user just deleted.
  if (removed && wasActive) {
    SETTINGS.dictionaryName[0] = '\0';
    SETTINGS.saveToFile();
  }

  RenderLock lock(*this);
  state_ = CATALOG_LIST;
}

void DictionaryDownloadActivity::activateSelected() {
  if (selectedIndex_ < 0 || selectedIndex_ >= listItemCount()) return;

  if (entries_[static_cast<size_t>(selectedIndex_)].installed) {
    RenderLock lock(*this);
    state_ = CONFIRM_DELETE;
    return;
  }

  downloadEntry(entries_[static_cast<size_t>(selectedIndex_)]);
  requestUpdateAndWait();
}

// --- Input handling ---

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect DictionaryDownloadActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void DictionaryDownloadActivity::loop() {
  if (state_ == CATALOG_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    if (!entries_.empty()) {
      int tappedIndex;
      switch (TouchListNav::tapRow(mappedInput, listRect(), listItemCount(), selectedIndex_,
                                   /*hasSubtitle=*/true, tappedIndex)) {
        case TouchListNav::TapResult::SelectionMoved:
          selectedIndex_ = tappedIndex;
          requestUpdate();
          return;
        case TouchListNav::TapResult::Activated:
          selectedIndex_ = tappedIndex;
          activateSelected();
          return;
        case TouchListNav::TapResult::None:
          break;
      }

      // Full Touch: a vertical swipe turns a page, matching the held side key.
      const int pageItems = GUI.listGeometry(listRect(), selectedIndex_, /*hasSubtitle=*/true).pageItems;
      if (TouchListNav::pageSwipe(mappedInput, listItemCount(), pageItems, selectedIndex_)) {
        requestUpdate();
        return;
      }
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < listItemCount() - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !entries_.empty()) {
      activateSelected();
      return;
    }
  } else if (state_ == CONFIRM_DELETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = CATALOG_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      deleteSelected();
      requestUpdate();
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = CATALOG_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // A catalog that never loaded leaves no list to go back to.
      if (entries_.empty()) {
        finish();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = CATALOG_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (entries_.empty()) {
        // Retry the catalog fetch itself.
        {
          RenderLock lock(*this);
          state_ = LOADING_CATALOG;
        }
        requestUpdateAndWait();
        if (fetchAndParseCatalog()) {
          RenderLock lock(*this);
          state_ = CATALOG_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
        return;
      }
      if (downloadingIndex_ >= 0 && downloadingIndex_ < listItemCount()) {
        downloadEntry(entries_[static_cast<size_t>(downloadingIndex_)]);
        requestUpdateAndWait();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = CATALOG_LIST;
      }
      requestUpdate();
    }
  }
}

// --- Rendering ---

std::string DictionaryDownloadActivity::formatSize(const size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void DictionaryDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICT_DOWNLOAD));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_CATALOG) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DICT_LOADING_LIST));
  } else if (state_ == CATALOG_LIST) {
    if (entries_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DICT_NONE_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer, listRect(), listItemCount(), selectedIndex_,
          [this](int index) -> std::string { return entries_[static_cast<size_t>(index)].title; },
          [this](int index) -> std::string { return entries_[static_cast<size_t>(index)].subtitle; }, nullptr,
          [this](int index) -> std::string {
            const auto& e = entries_[static_cast<size_t>(index)];
            if (e.installed) return tr(STR_INSTALLED);
            return e.bytes > 0 ? formatSize(e.bytes) : std::string();
          },
          true);

      const bool installed = entries_[static_cast<size_t>(selectedIndex_)].installed;
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), installed ? tr(STR_DELETE) : tr(STR_DOWNLOAD),
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == CONFIRM_DELETE) {
    const auto& entry = entries_[static_cast<size_t>(selectedIndex_)];
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DICT_DELETE_CONFIRM), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, entry.title.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DELETE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    const auto& entry = entries_[static_cast<size_t>(downloadingIndex_)];

    char status[96];
    snprintf(status, sizeof(status), "%s %s (%u/%u)", tr(STR_DOWNLOADING), entry.title.c_str(),
             static_cast<unsigned>(currentFileIndex_ + 1), static_cast<unsigned>(currentFileTotal_));
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, status);

    // Static frame — the framebuffer is lent to the transfer's TLS session, so
    // there is no live bar; the (i/n) counter advances between files.
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, tr(STR_DOWNLOAD_WAIT));

    // The hint bar is what makes the cancel reachable, not merely legible: it
    // names the right physical button on the button boards, and on the X4 Pro
    // it publishes a tappable Back slot that outlives the frozen screen
    // (ActionBar's slots stand until the next render, and there is no render
    // until the transfer ends). pollForCancel() polls for exactly that press.
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DICT_INSTALLED_OK), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DICT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
