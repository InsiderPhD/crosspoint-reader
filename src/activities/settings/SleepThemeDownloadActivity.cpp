#include "SleepThemeDownloadActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <StreamingJsonParser.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/WifiPowerSaveGuard.h"
#include "sleeptheme/SleepThemeRenderer.h"
#include "util/HardcoverSync.h"
#include "util/TouchListNav.h"

namespace {

// Scratch file the catalog lands in before it is parsed: streaming the parse
// during the live TLS session would carry both heap costs at once.
constexpr const char* CATALOG_TMP = "/sleep_theme_catalog.tmp";
// The previewed theme's screenshot as fetched, and its fitted conversion. At
// the card root beside CATALOG_TMP, not under /sleep-themes, so the picker
// never sees them.
constexpr const char* PREVIEW_PNG = "/sleep_theme_preview.png";
constexpr const char* PREVIEW_BMP = "/sleep_theme_preview.bmp";

// Every row costs heap for the life of the screen.
constexpr size_t MAX_CATALOG_ENTRIES = 48;
// theme.bin, bg.bmp, mask.bmp, front.bmp and four fonts.
constexpr size_t MAX_THEME_FILES = 8;
// Matches CrossPointSettings::sleepTheme[32].
constexpr size_t MAX_ID_LEN = 31;
constexpr size_t MAX_NAME_LEN = 48;
constexpr size_t MAX_DESCRIPTION_LEN = 96;
// A build's version: a short content hash.
constexpr size_t MAX_VERSION_LEN = 16;
// Written into a theme folder by an install: the version it installed.
constexpr const char* VERSION_FILE = "store.ver";

// ^[a-z0-9][a-z0-9-]{0,30}$: the id becomes a folder name and the setting value.
bool isValidId(const std::string& id) {
  if (id.empty() || id.size() > MAX_ID_LEN || id[0] == '-') return false;
  return std::all_of(id.begin(), id.end(),
                     [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// ^(theme\.bin|bg\.bmp|mask\.bmp|front\.bmp|f[0-3]\.cpfont)$: the only files a theme
// folder holds, so a catalog can never write anywhere else.
bool isThemeFileName(const std::string& name) {
  if (name == "theme.bin" || name == "bg.bmp" || name == "mask.bmp" || name == "front.bmp") return true;
  return name.size() == 9 && name[0] == 'f' && name[1] >= '0' && name[1] <= '3' && name.compare(2, 7, ".cpfont") == 0;
}

// "<long>x<short>" of this panel, the key the store builds themes under.
void buildKeyFor(const GfxRenderer& renderer, char* out, const size_t outSize) {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  snprintf(out, outSize, "%dx%d", std::max(w, h), std::min(w, h));
}

// Catalog directory: the catalog URL up to and including its last '/'.
std::string catalogBase() {
  const std::string url = SLEEP_THEME_CATALOG_URL;
  const size_t slash = url.rfind('/');
  return slash == std::string::npos ? url : url.substr(0, slash + 1);
}

std::string themeDir(const std::string& id) { return std::string(SleepThemeRenderer::THEMES_DIR) + "/" + id; }
std::string partDir(const std::string& id) { return std::string(SleepThemeRenderer::THEMES_DIR) + "/." + id + ".part"; }

// Converts a fetched preview PNG to a 2-bit BMP fitted within boxW x boxH, then
// deletes the PNG unless keepPng. Runs after the TLS session has closed, so the
// PNG inflate and its row buffers (sized by the ~500 px preview width) never
// share the heap with it; all of it is freed before this returns.
bool convertPreview(const char* pngPath, const std::string& bmpPath, const int boxW, const int boxH,
                    const bool keepPng) {
  bool converted = false;
  FsFile png;
  if (Storage.openFileForRead("STD", pngPath, png)) {
    FsFile bmp;
    if (Storage.openFileForWrite("STD", bmpPath, bmp)) {
      // Fit (not crop): the whole screen, shrunk.
      converted = PngToBmpConverter::pngFileToBmpStreamWithSize(png, bmp, boxW, boxH, /*crop=*/false);
      bmp.flush();
      bmp.close();
    }
    png.close();
  }
  if (!keepPng) Storage.remove(pngPath);
  LOG_DBG("STD", "Preview -> %s: %s; heap free=%u largest=%u", bmpPath.c_str(), converted ? "ok" : "failed",
          ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  if (!converted && Storage.exists(bmpPath.c_str())) Storage.remove(bmpPath.c_str());
  return converted;
}

// Streaming catalog parser (SAX, like the dictionary catalog's): entries land
// straight in the output vector, and only this device's build is kept.
//
//   { "version": 1, "themes": [
//       { "id": "...", "name": "...", "description": "...", "orientation": "...",
//         "overlay": false,
//         "builds": { "800x480": { "version": "...",
//                                  "files": [ { "name": "theme.bin", "size": N } ] } } } ] }
class ThemeCatalogJsonStream final : public Stream {
 public:
  ThemeCatalogJsonStream(std::vector<SleepThemeDownloadActivity::CatalogEntry>& outEntries, const char* buildKey)
      : outEntries_(outEntries),
        buildKey_(buildKey),
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
    THEMES,
    ID,
    NAME,
    DESCRIPTION,
    BUILDS,
    MY_BUILD,
    VERSION,
    FILES,
    FILE_NAME,
    FILE_SIZE
  };

  static void sOnKey(void* ctx, const char* key, size_t len) {
    static_cast<ThemeCatalogJsonStream*>(ctx)->onKey(key, len);
  }
  static void sOnString(void* ctx, const char* value, size_t len) {
    static_cast<ThemeCatalogJsonStream*>(ctx)->onString(value, len);
  }
  static void sOnNumber(void* ctx, const char* value, size_t len) {
    static_cast<ThemeCatalogJsonStream*>(ctx)->onNumber(value, len);
  }
  static void sOnBool(void* ctx, bool /*value*/) {
    static_cast<ThemeCatalogJsonStream*>(ctx)->lastKey_ = LastKey::NONE;
  }
  static void sOnNull(void* ctx) { static_cast<ThemeCatalogJsonStream*>(ctx)->lastKey_ = LastKey::NONE; }
  static void sOnObjectStart(void* ctx) { static_cast<ThemeCatalogJsonStream*>(ctx)->onObjectStart(); }
  static void sOnObjectEnd(void* ctx) { static_cast<ThemeCatalogJsonStream*>(ctx)->onObjectEnd(); }
  static void sOnArrayStart(void* ctx) { static_cast<ThemeCatalogJsonStream*>(ctx)->onArrayStart(); }
  static void sOnArrayEnd(void* ctx) { static_cast<ThemeCatalogJsonStream*>(ctx)->onArrayEnd(); }

  static bool keyIs(const char* key, size_t len, const char* expected) {
    return strlen(expected) == len && memcmp(key, expected, len) == 0;
  }

  // Depths below are the depth INSIDE the container (depth_ after its start).
  void onKey(const char* key, size_t len) {
    lastKey_ = LastKey::NONE;
    if (inFile_ && depth_ == fileDepth_) {
      if (keyIs(key, len, "name"))
        lastKey_ = LastKey::FILE_NAME;
      else if (keyIs(key, len, "size"))
        lastKey_ = LastKey::FILE_SIZE;
    } else if (inMyBuild_ && depth_ == myBuildDepth_) {
      if (keyIs(key, len, "files"))
        lastKey_ = LastKey::FILES;
      else if (keyIs(key, len, "version"))
        lastKey_ = LastKey::VERSION;
    } else if (inBuilds_ && depth_ == buildsDepth_) {
      if (keyIs(key, len, buildKey_)) lastKey_ = LastKey::MY_BUILD;
    } else if (inTheme_ && depth_ == themeDepth_) {
      if (keyIs(key, len, "id"))
        lastKey_ = LastKey::ID;
      else if (keyIs(key, len, "name"))
        lastKey_ = LastKey::NAME;
      else if (keyIs(key, len, "description"))
        lastKey_ = LastKey::DESCRIPTION;
      else if (keyIs(key, len, "builds"))
        lastKey_ = LastKey::BUILDS;
    } else if (rootSeen_ && depth_ == 1) {
      if (keyIs(key, len, "themes")) lastKey_ = LastKey::THEMES;
    }
  }

  void onString(const char* value, size_t len) {
    switch (lastKey_) {
      case LastKey::ID:
        current_.id.assign(value, std::min(len, MAX_ID_LEN + 1));  // one over: an over-long id fails validation
        break;
      case LastKey::NAME:
        current_.name.assign(value, std::min(len, MAX_NAME_LEN));
        break;
      case LastKey::DESCRIPTION:
        current_.description.assign(value, std::min(len, MAX_DESCRIPTION_LEN));
        break;
      case LastKey::FILE_NAME:
        currentFile_.name.assign(value, std::min<size_t>(len, 16));
        break;
      case LastKey::VERSION:
        current_.version.assign(value, std::min(len, MAX_VERSION_LEN));
        break;
      default:
        break;
    }
    lastKey_ = LastKey::NONE;
  }

  void onNumber(const char* value, size_t /*len*/) {
    // Parser tokens are null-terminated, so strtoul is safe on the raw pointer.
    if (lastKey_ == LastKey::FILE_SIZE) currentFile_.size = static_cast<uint32_t>(strtoul(value, nullptr, 10));
    lastKey_ = LastKey::NONE;
  }

  void onObjectStart() {
    if (depth_ == 0) {
      rootSeen_ = true;
    } else if (inThemes_ && !inTheme_ && depth_ == themesDepth_) {
      current_ = SleepThemeDownloadActivity::CatalogEntry{};
      current_.files.reserve(4);
      filesOk_ = true;
      hasBuild_ = false;
      inTheme_ = true;
      themeDepth_ = depth_ + 1;
    } else if (inTheme_ && !inBuilds_ && lastKey_ == LastKey::BUILDS && depth_ == themeDepth_) {
      inBuilds_ = true;
      buildsDepth_ = depth_ + 1;
    } else if (inBuilds_ && !inMyBuild_ && lastKey_ == LastKey::MY_BUILD && depth_ == buildsDepth_) {
      inMyBuild_ = true;
      hasBuild_ = true;
      myBuildDepth_ = depth_ + 1;
    } else if (inFiles_ && !inFile_ && depth_ == filesDepth_) {
      currentFile_ = SleepThemeDownloadActivity::CatalogFile{};
      inFile_ = true;
      fileDepth_ = depth_ + 1;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onObjectEnd() {
    if (inFile_ && depth_ == fileDepth_) {
      // Every file name is concatenated into an SD path: validate it here, once.
      if (!isThemeFileName(currentFile_.name) || current_.files.size() >= MAX_THEME_FILES) {
        filesOk_ = false;
      } else {
        current_.bytes += currentFile_.size;
        current_.files.push_back(std::move(currentFile_));
      }
      inFile_ = false;
    } else if (inMyBuild_ && depth_ == myBuildDepth_) {
      inMyBuild_ = false;
    } else if (inBuilds_ && depth_ == buildsDepth_) {
      inBuilds_ = false;
    } else if (inTheme_ && depth_ == themeDepth_) {
      finishTheme();
      inTheme_ = false;
    }
    if (depth_ > 0) --depth_;
    if (rootSeen_ && depth_ == 0) rootClosed_ = true;
    lastKey_ = LastKey::NONE;
  }

  void onArrayStart() {
    if (lastKey_ == LastKey::THEMES && depth_ == 1) {
      inThemes_ = true;
      themesDepth_ = depth_ + 1;
    } else if (inMyBuild_ && lastKey_ == LastKey::FILES && depth_ == myBuildDepth_) {
      inFiles_ = true;
      filesDepth_ = depth_ + 1;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onArrayEnd() {
    if (inFiles_ && depth_ == filesDepth_) {
      inFiles_ = false;
    } else if (inThemes_ && depth_ == themesDepth_) {
      inThemes_ = false;
    }
    if (depth_ > 0) --depth_;
    lastKey_ = LastKey::NONE;
  }

  // Keeps a theme only when it can actually be installed here: a valid id, a
  // build for this panel, and a file list that includes theme.bin.
  void finishTheme() {
    const bool hasThemeBin =
        std::any_of(current_.files.begin(), current_.files.end(),
                    [](const SleepThemeDownloadActivity::CatalogFile& f) { return f.name == "theme.bin"; });
    if (!isValidId(current_.id) || !hasBuild_ || !filesOk_ || !hasThemeBin) {
      LOG_DBG("STD", "Skipping catalog theme '%s' (no usable %s build)", current_.id.c_str(), buildKey_);
      return;
    }
    if (outEntries_.size() >= MAX_CATALOG_ENTRIES) return;
    if (current_.name.empty()) current_.name = current_.id;
    outEntries_.push_back(std::move(current_));
  }

  std::vector<SleepThemeDownloadActivity::CatalogEntry>& outEntries_;
  const char* buildKey_;
  StreamingJsonParser parser_;
  SleepThemeDownloadActivity::CatalogEntry current_;
  SleepThemeDownloadActivity::CatalogFile currentFile_;
  LastKey lastKey_ = LastKey::NONE;
  size_t bytesRead_ = 0;
  uint8_t depth_ = 0;
  uint8_t themesDepth_ = 0;
  uint8_t themeDepth_ = 0;
  uint8_t buildsDepth_ = 0;
  uint8_t myBuildDepth_ = 0;
  uint8_t filesDepth_ = 0;
  uint8_t fileDepth_ = 0;
  bool rootSeen_ = false;
  bool rootClosed_ = false;
  bool inThemes_ = false;
  bool inTheme_ = false;
  bool inBuilds_ = false;
  bool inMyBuild_ = false;
  bool inFiles_ = false;
  bool inFile_ = false;
  bool hasBuild_ = false;
  bool filesOk_ = true;
};

}  // namespace

SleepThemeDownloadActivity::SleepThemeDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("SleepThemeDownload", renderer, mappedInput) {}

// --- Lifecycle ---

void SleepThemeDownloadActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("STD", "Heap at entry: free=%u largest=%u", ESP.getFreeHeap(),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  HardcoverSync::preempt();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void SleepThemeDownloadActivity::onExit() {
  Activity::onExit();
  discardPreview();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void SleepThemeDownloadActivity::onWifiSelectionComplete(const bool success) {
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

bool SleepThemeDownloadActivity::fetchAndParseCatalog() {
  // Shed cached glyphs before the TLS handshake; they re-warm on the next paint.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCachesDeep();
  }

  // Two serial phases so the heap never carries both costs at once: fetch to
  // SD inside the framebuffer borrow, then stream-parse on a quiet heap.
  auto dlResult = HttpDownloader::HTTP_ERROR;
  {
    TlsFramebufferBorrow borrow(renderer);
    dlResult = HttpDownloader::downloadToFile(SLEEP_THEME_CATALOG_URL, CATALOG_TMP, nullptr,
                                              /*allowConfiguredAuth=*/false);
  }
  if (dlResult != HttpDownloader::OK) {
    LOG_ERR("STD", "Failed to fetch catalog from %s (%d)", SLEEP_THEME_CATALOG_URL, dlResult);
    errorMessage_ = tr(STR_SLEEP_THEME_LIST_FAILED);
    Storage.remove(CATALOG_TMP);
    return false;
  }

  FsFile catalogFile;
  if (!Storage.openFileForRead("STD", CATALOG_TMP, catalogFile)) {
    LOG_ERR("STD", "Failed to open temp catalog");
    Storage.remove(CATALOG_TMP);
    errorMessage_ = tr(STR_SLEEP_THEME_LIST_FAILED);
    return false;
  }

  char buildKey[16];
  buildKeyFor(renderer, buildKey, sizeof(buildKey));

  entries_.reserve(16);
  ThemeCatalogJsonStream stream(entries_, buildKey);
  uint8_t chunk[256];
  int got;
  while ((got = catalogFile.read(chunk, sizeof(chunk))) > 0) {
    stream.write(chunk, static_cast<size_t>(got));
  }
  catalogFile.close();
  Storage.remove(CATALOG_TMP);

  if (!stream.ok()) {
    LOG_ERR("STD", "Catalog streaming parse error after %zu bytes", stream.bytesRead());
    errorMessage_ = tr(STR_SLEEP_THEME_LIST_INVALID);
    entries_.clear();
    return false;
  }

  refreshInstalledFlags();
  LOG_DBG("STD", "Catalog loaded: %zu themes for %s", entries_.size(), buildKey);
  return true;
}

// A theme counts as installed when its theme.bin is in place: the same test
// SleepThemeSelectActivity lists by. It is out of date when the version its
// install recorded is not this build's (no record counts as out of date).
void SleepThemeDownloadActivity::refreshInstalledFlags() {
  for (auto& entry : entries_) {
    const std::string dir = themeDir(entry.id);
    entry.installed = Storage.exists((dir + "/theme.bin").c_str());
    entry.updateAvailable = false;
    if (!entry.installed || entry.version.empty()) continue;
    char installedVersion[MAX_VERSION_LEN + 1] = "";
    Storage.readFileToBuffer((dir + "/" + VERSION_FILE).c_str(), installedVersion, sizeof(installedVersion));
    entry.updateAvailable = entry.version != installedVersion;
  }
}

int SleepThemeDownloadActivity::updateCount() const {
  return static_cast<int>(
      std::count_if(entries_.begin(), entries_.end(), [](const CatalogEntry& e) { return e.updateAvailable; }));
}

SleepThemeDownloadActivity::CatalogEntry* SleepThemeDownloadActivity::entryAtRow(const int row) {
  const int index = row - entryRowOffset();
  if (index < 0 || index >= static_cast<int>(entries_.size())) return nullptr;
  return &entries_[static_cast<size_t>(index)];
}

// Reinstalls every out-of-date theme, one after another, stopping at the
// first error or cancel.
void SleepThemeDownloadActivity::updateAll() {
  for (auto& entry : entries_) {
    if (!entry.updateAvailable) continue;
    if (!downloadEntry(entry)) return;
  }
  RenderLock lock(*this);
  selectedIndex_ = 0;
}

// --- Download ---

void SleepThemeDownloadActivity::pollForCancel() {
  // Runs inside the transfer with the render lock held: poll, set a flag,
  // nothing else. Throttled: each poll reaches the GT911 over I2C on the X4 Pro.
  const unsigned long now = millis();
  if (now - lastCancelPollMs_ < 250) return;
  lastCancelPollMs_ = now;

  mappedInput.update();
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    LOG_INF("STD", "Cancel requested");
    cancelRequested_ = true;
  }
}

bool SleepThemeDownloadActivity::downloadEntry(CatalogEntry& entry) {
  const bool isUpdate = entry.installed;
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingIndex_ = static_cast<int>(&entry - entries_.data());
    currentFileIndex_ = 0;
    currentFileTotal_ = entry.files.size();
  }
  cancelRequested_ = false;
  lastCancelPollMs_ = millis();

  // Fresh staging folder: a leftover from an interrupted install must not mix
  // its files into this one.
  const std::string staging = partDir(entry.id);
  if (Storage.exists(staging.c_str())) Storage.removeDir(staging.c_str());
  if (!Storage.mkdir(staging.c_str())) {
    LOG_ERR("STD", "Could not create %s", staging.c_str());
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_SLEEP_THEME_INSTALL_DIR_FAILED);
    return false;
  }

  // Keep the radio awake for the whole theme (power-save on an idle link
  // surfaces as a TCP connect failure on the first request).
  WifiPowerSaveGuard wifiPowerSaveGuard;

  char buildKey[16];
  buildKeyFor(renderer, buildKey, sizeof(buildKey));
  const std::string base = catalogBase() + buildKey + "/" + entry.id + "/";

  for (size_t i = 0; i < entry.files.size(); i++) {
    const auto& file = entry.files[i];
    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
    }
    // The (i/n) frame must be on the panel before the screen freezes for this
    // file's TLS session.
    requestUpdateAndWait();

    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCachesDeep();
    }
    LOG_DBG("STD", "Heap before %s: free=%u largest=%u", file.name.c_str(), ESP.getFreeHeap(),
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));

    const std::string url = base + file.name;
    const std::string destPath = staging + "/" + file.name;

    auto result = HttpDownloader::HTTP_ERROR;
    constexpr int kMaxFileAttempts = 3;
    for (int attempt = 1; attempt <= kMaxFileAttempts; attempt++) {
      TlsFramebufferBorrow borrow(renderer);
      result = HttpDownloader::downloadToFile(
          url, destPath, [this](size_t, size_t) { pollForCancel(); },
          /*allowConfiguredAuth=*/false, /*expectedSize=*/file.size, &cancelRequested_);
      if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED) break;
      LOG_ERR("STD", "Download attempt %d/%d failed: %s (%d)", attempt, kMaxFileAttempts, file.name.c_str(), result);
    }

    if (result != HttpDownloader::OK) {
      Storage.removeDir(staging.c_str());
      RenderLock lock(*this);
      if (result == HttpDownloader::ABORTED) {
        state_ = CATALOG_LIST;
      } else {
        LOG_ERR("STD", "Download failed: %s (%d)", file.name.c_str(), result);
        state_ = ERROR;
        errorMessage_ = std::string(tr(STR_DOWNLOAD_FAILED)) + ": " + file.name;
      }
      return false;
    }
  }

  // The store's screenshot, saved as the pickers' preview. Optional: a theme
  // whose preview is missing or fails to convert still installs. When the
  // store's preview screen already fetched it for this theme, that PNG is
  // reused instead of fetched again.
  {
    const std::string stagedPng = staging + "/preview.png";
    bool havePng = false;
    if (previewId_ == entry.id && Storage.exists(PREVIEW_PNG)) {
      havePng = Storage.rename(PREVIEW_PNG, stagedPng.c_str());
    }
    if (!havePng) {
      if (auto* fcm = renderer.getFontCacheManager()) {
        fcm->clearCachesDeep();
      }
      auto result = HttpDownloader::HTTP_ERROR;
      {
        TlsFramebufferBorrow borrow(renderer);
        result = HttpDownloader::downloadToFile(
            base + "preview.png", stagedPng, [this](size_t, size_t) { pollForCancel(); },
            /*allowConfiguredAuth=*/false, /*expectedSize=*/0, &cancelRequested_);
      }
      if (result == HttpDownloader::ABORTED) {
        Storage.removeDir(staging.c_str());
        RenderLock lock(*this);
        state_ = CATALOG_LIST;
        return false;
      }
      havePng = result == HttpDownloader::OK;
      if (!havePng) LOG_ERR("STD", "No preview for %s (%d); installing without one", entry.id.c_str(), result);
    }
    if (havePng) {
      const int side = std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
      convertPreview(stagedPng.c_str(), staging + "/" + SleepThemeRenderer::PREVIEW_FILE, side, side,
                     /*keepPng=*/false);
    }
  }

  // Which build this is, for the update check. Written before the swap so a
  // theme never lands without it.
  if (!entry.version.empty() && !Storage.writeFile((staging + "/" + VERSION_FILE).c_str(), entry.version.c_str())) {
    LOG_ERR("STD", "Could not write %s/%s", staging.c_str(), VERSION_FILE);
  }

  // Swap the complete theme into place. An update replaces the old folder;
  // between the remove and the rename the theme is simply absent, never partial.
  const std::string finalDir = themeDir(entry.id);
  if (Storage.exists(finalDir.c_str()) && !Storage.removeDir(finalDir.c_str())) {
    LOG_ERR("STD", "Could not remove the old %s", finalDir.c_str());
  }
  if (!Storage.rename(staging.c_str(), finalDir.c_str())) {
    LOG_ERR("STD", "Could not move %s into place", staging.c_str());
    Storage.removeDir(staging.c_str());
    refreshInstalledFlags();
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_SLEEP_THEME_INSTALL_DIR_FAILED);
    return false;
  }
  refreshInstalledFlags();

  // A new theme that lands while the configured one isn't on the card becomes
  // the sleep screen: otherwise someone who came here from an empty picker
  // downloads a theme and still sleeps on the built-in fallback. Updates never
  // switch it, and installing the configured theme itself needs no write.
  const std::string configuredPath = themeDir(SETTINGS.sleepTheme) + "/theme.bin";
  if (!isUpdate && entry.id != SETTINGS.sleepTheme && !SleepThemeRenderer::isBuiltIn(SETTINGS.sleepTheme) &&
      (SETTINGS.sleepTheme[0] == '\0' || !Storage.exists(configuredPath.c_str()))) {
    strncpy(SETTINGS.sleepTheme, entry.id.c_str(), sizeof(SETTINGS.sleepTheme) - 1);
    SETTINGS.sleepTheme[sizeof(SETTINGS.sleepTheme) - 1] = '\0';
    SETTINGS.saveToFile();
    LOG_INF("STD", "Auto-selected first sleep theme: %s", entry.id.c_str());
  }

  RenderLock lock(*this);
  state_ = COMPLETE;
  return true;
}

void SleepThemeDownloadActivity::deleteSelected() {
  const CatalogEntry* selected = entryAtRow(selectedIndex_);
  if (selected == nullptr) return;
  const auto& entry = *selected;

  const std::string dir = themeDir(entry.id);
  const bool removed = Storage.removeDir(dir.c_str());
  if (!removed) LOG_ERR("STD", "Failed to remove %s", dir.c_str());
  refreshInstalledFlags();
  // The Update All row can vanish with the theme: keep the cursor in range.
  selectedIndex_ = std::min(selectedIndex_, listItemCount() - 1);

  // Don't leave the setting naming a folder that is gone (light and dark
  // carry on as the built-in screen): fall back to light.
  if (removed && strncmp(SETTINGS.sleepTheme, entry.id.c_str(), sizeof(SETTINGS.sleepTheme) - 1) == 0 &&
      !SleepThemeRenderer::isBuiltIn(SETTINGS.sleepTheme)) {
    strncpy(SETTINGS.sleepTheme, "light", sizeof(SETTINGS.sleepTheme) - 1);
    SETTINGS.saveToFile();
  }

  RenderLock lock(*this);
  state_ = CATALOG_LIST;
}

void SleepThemeDownloadActivity::activateSelected() {
  if (isUpdateAllRow(selectedIndex_)) {
    updateAll();
    requestUpdateAndWait();
    return;
  }
  CatalogEntry* entry = entryAtRow(selectedIndex_);
  if (entry == nullptr) return;

  // Installed and current: Confirm offers to delete it. Out of date or not
  // installed: Confirm downloads it.
  if (entry->installed && !entry->updateAvailable) {
    RenderLock lock(*this);
    state_ = CONFIRM_DELETE;
    return;
  }

  // Not installed or out of date: show what it looks like first.
  openPreview(*entry);
}

// --- Preview ---

void SleepThemeDownloadActivity::discardPreview() {
  if (Storage.exists(PREVIEW_PNG)) Storage.remove(PREVIEW_PNG);
  if (Storage.exists(PREVIEW_BMP)) Storage.remove(PREVIEW_BMP);
  previewId_.clear();
}

Rect SleepThemeDownloadActivity::previewRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect body = listRect();
  return Rect{body.x + metrics.contentSidePadding, body.y, body.width - 2 * metrics.contentSidePadding, body.height};
}

void SleepThemeDownloadActivity::openPreview(const CatalogEntry& entry) {
  {
    RenderLock lock(*this);
    state_ = PREVIEW_LOADING;
    previewReady_ = false;
  }
  // The loading frame must be on the panel before the screen freezes for the
  // fetch's TLS session.
  requestUpdateAndWait();

  const bool ready = fetchPreview(entry);
  {
    RenderLock lock(*this);
    if (cancelRequested_) {
      discardPreview();
      state_ = CATALOG_LIST;
    } else {
      previewReady_ = ready;
      state_ = PREVIEW;
    }
  }
  requestUpdate();
}

// Two serial phases like the catalog: fetch the PNG inside the framebuffer
// borrow, then convert it on a quiet heap. The conversion (one PNG inflate,
// a few row buffers sized by the ~500 px preview width) is freed before it
// returns; only the fitted BMP on the card outlives it, read row by row on
// each paint. False = nothing to show (missing, failed, or cancelled).
bool SleepThemeDownloadActivity::fetchPreview(const CatalogEntry& entry) {
  discardPreview();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCachesDeep();
  }

  char buildKey[16];
  buildKeyFor(renderer, buildKey, sizeof(buildKey));
  const std::string url = catalogBase() + buildKey + "/" + entry.id + "/preview.png";

  cancelRequested_ = false;
  lastCancelPollMs_ = millis();
  auto result = HttpDownloader::HTTP_ERROR;
  {
    WifiPowerSaveGuard wifiPowerSaveGuard;
    TlsFramebufferBorrow borrow(renderer);
    result = HttpDownloader::downloadToFile(
        url, PREVIEW_PNG, [this](size_t, size_t) { pollForCancel(); },
        /*allowConfiguredAuth=*/false, /*expectedSize=*/0, &cancelRequested_);
  }
  if (result != HttpDownloader::OK) {
    if (result != HttpDownloader::ABORTED) LOG_ERR("STD", "Preview fetch failed: %s (%d)", url.c_str(), result);
    discardPreview();
    return false;
  }

  const Rect box = previewRect();
  // The PNG stays until the preview closes: Download installs it as the theme's
  // own preview instead of fetching it again.
  previewId_ = entry.id;
  return convertPreview(PREVIEW_PNG, PREVIEW_BMP, box.width, box.height, /*keepPng=*/true);
}

// --- Input handling ---

// List body between the header and the button hints. Shared by render() and
// loop()'s tap hit-testing so the two can never disagree.
Rect SleepThemeDownloadActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void SleepThemeDownloadActivity::loop() {
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
  } else if (state_ == PREVIEW) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      discardPreview();
      {
        RenderLock lock(*this);
        state_ = CATALOG_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      CatalogEntry* entry = entryAtRow(selectedIndex_);
      if (entry == nullptr) {
        discardPreview();
        RenderLock lock(*this);
        state_ = CATALOG_LIST;
        return;
      }
      // downloadEntry() moves the fetched PNG into the theme; drop the rest.
      downloadEntry(*entry);
      discardPreview();
      requestUpdateAndWait();
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = CATALOG_LIST;
        // An update can remove the Update All row above the cursor.
        selectedIndex_ = std::max(0, std::min(selectedIndex_, listItemCount() - 1));
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
        {
          RenderLock lock(*this);
          state_ = LOADING_CATALOG;
        }
        requestUpdateAndWait();
        const bool loaded = fetchAndParseCatalog();
        {
          RenderLock lock(*this);
          state_ = loaded ? CATALOG_LIST : ERROR;
          selectedIndex_ = 0;
        }
        requestUpdate();
        return;
      }
      if (downloadingIndex_ >= 0 && downloadingIndex_ < static_cast<int>(entries_.size())) {
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

std::string SleepThemeDownloadActivity::formatSize(const size_t bytes) {
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

void SleepThemeDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // The preview screens are titled with the theme they show.
  const char* title = tr(STR_SLEEP_THEME_STORE);
  if (state_ == PREVIEW_LOADING || state_ == PREVIEW) {
    if (const CatalogEntry* e = entryAtRow(selectedIndex_)) title = e->name.c_str();
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_CATALOG) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_SLEEP_THEME_LOADING_LIST));
  } else if (state_ == CATALOG_LIST) {
    if (entries_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_SLEEP_THEME_NONE_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer, listRect(), listItemCount(), selectedIndex_,
          [this](int row) -> std::string {
            const CatalogEntry* e = entryAtRow(row);
            if (e != nullptr) return e->name;
            char label[48];
            snprintf(label, sizeof(label), tr(STR_SLEEP_THEME_UPDATE_ALL), updateCount());
            return label;
          },
          [this](int row) -> std::string {
            const CatalogEntry* e = entryAtRow(row);
            return e != nullptr ? e->description : std::string();
          },
          nullptr,
          [this](int row) -> std::string {
            const CatalogEntry* e = entryAtRow(row);
            if (e == nullptr) return "";
            if (e->updateAvailable) return tr(STR_UPDATE_AVAILABLE);
            if (e->installed) return tr(STR_INSTALLED);
            return e->bytes > 0 ? formatSize(e->bytes) : std::string();
          },
          true);

      const CatalogEntry* selected = entryAtRow(selectedIndex_);
      const char* action = tr(STR_UPDATE);
      if (selected != nullptr && !selected->updateAvailable) {
        action = selected->installed ? tr(STR_DELETE) : tr(STR_DOWNLOAD);
      }
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), action, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == CONFIRM_DELETE) {
    const CatalogEntry* entry = entryAtRow(selectedIndex_);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_SLEEP_THEME_DELETE_CONFIRM), true,
                              EpdFontFamily::BOLD);
    if (entry != nullptr) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, entry->name.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DELETE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == PREVIEW_LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_SLEEP_THEME_LOADING_PREVIEW));
    // Frozen frame during the fetch, as in DOWNLOADING: Back cancels.
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == PREVIEW) {
    const Rect box = previewRect();
    // Dark mode inverts the whole screen below: draw the negative so the
    // picture comes out as the theme really looks.
    const bool drawn = previewReady_ && SleepThemeRenderer::drawPreview(renderer, PREVIEW_BMP, box.x, box.y, box.width,
                                                                        box.height, SETTINGS.darkMode);
    if (!drawn) renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_SLEEP_THEME_NO_PREVIEW));
    const CatalogEntry* entry = entryAtRow(selectedIndex_);
    const char* action = entry != nullptr && entry->updateAvailable ? tr(STR_UPDATE) : tr(STR_DOWNLOAD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), action, "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    const auto& entry = entries_[static_cast<size_t>(downloadingIndex_)];
    char status[96];
    snprintf(status, sizeof(status), "%s %s (%u/%u)", tr(STR_DOWNLOADING), entry.name.c_str(),
             static_cast<unsigned>(currentFileIndex_ + 1), static_cast<unsigned>(currentFileTotal_));
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, status);
    // Static frame: the framebuffer is lent to the transfer's TLS session.
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, tr(STR_DOWNLOAD_WAIT));
    // The hint bar keeps Cancel reachable while the screen is frozen (on the X4
    // Pro its tappable Back slot outlives the frame). See pollForCancel().
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_SLEEP_THEME_INSTALLED_OK), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_SLEEP_THEME_INSTALL_FAILED), true,
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
