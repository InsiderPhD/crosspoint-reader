#include "FontDownloadActivity.h"

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

#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/TouchListNav.h"

namespace {
// Streaming manifest parser (same SAX pattern as the BookFusion clients): the
// HTTP body feeds StreamingJsonParser chunk-by-chunk and family entries land
// directly in the output vector. No DOM, no temp file — an ArduinoJson DOM of
// the manifest runs 2-3x the source (a 15KB manifest wanted 30-45KB) and
// aborted the firmware on this activity's ~28KB heap (std::string/new under
// -fno-exceptions; observed on-device).
//
// Manifest shape:
//   { "version": N, "baseUrl": "...", "families": [
//       { "name": "...", "description": "...", "styles": ["..."],
//         "files": [ { "name": "...", "size": N } ] } ] }
class FontManifestJsonStream final : public Stream {
 public:
  FontManifestJsonStream(std::vector<FontDownloadActivity::ManifestFamily>& outFamilies, std::string& outBaseUrl)
      : outFamilies_(outFamilies),
        outBaseUrl_(outBaseUrl),
        parser_(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                              sOnArrayStart, sOnArrayEnd}) {
    outFamilies_.clear();
    outBaseUrl_.clear();
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

  int version() const { return version_; }
  size_t bytesRead() const { return bytesRead_; }
  bool ok() const { return bytesRead_ > 0 && rootSeen_ && rootClosed_ && depth_ == 0 && !parser_.hasError(); }

 private:
  enum class LastKey : uint8_t {
    NONE,
    VERSION,
    BASE_URL,
    FAMILIES,
    NAME,
    DESCRIPTION,
    STYLES,
    FILES,
    FILE_NAME,
    FILE_SIZE,
  };

  static void sOnKey(void* ctx, const char* key, size_t len) {
    static_cast<FontManifestJsonStream*>(ctx)->onKey(key, len);
  }
  static void sOnString(void* ctx, const char* value, size_t len) {
    static_cast<FontManifestJsonStream*>(ctx)->onString(value, len);
  }
  static void sOnNumber(void* ctx, const char* value, size_t len) {
    static_cast<FontManifestJsonStream*>(ctx)->onNumber(value, len);
  }
  static void sOnBool(void* ctx, bool /*value*/) {
    static_cast<FontManifestJsonStream*>(ctx)->lastKey_ = LastKey::NONE;
  }
  static void sOnNull(void* ctx) { static_cast<FontManifestJsonStream*>(ctx)->lastKey_ = LastKey::NONE; }
  static void sOnObjectStart(void* ctx) { static_cast<FontManifestJsonStream*>(ctx)->onObjectStart(); }
  static void sOnObjectEnd(void* ctx) { static_cast<FontManifestJsonStream*>(ctx)->onObjectEnd(); }
  static void sOnArrayStart(void* ctx) { static_cast<FontManifestJsonStream*>(ctx)->onArrayStart(); }
  static void sOnArrayEnd(void* ctx) { static_cast<FontManifestJsonStream*>(ctx)->onArrayEnd(); }

  static bool keyIs(const char* key, size_t len, const char* expected) {
    return strlen(expected) == len && memcmp(key, expected, len) == 0;
  }

  void onKey(const char* key, size_t len) {
    lastKey_ = LastKey::NONE;
    if (inFile_ && depth_ == fileDepth_) {
      if (keyIs(key, len, "name"))
        lastKey_ = LastKey::FILE_NAME;
      else if (keyIs(key, len, "size"))
        lastKey_ = LastKey::FILE_SIZE;
    } else if (inFamily_ && depth_ == familyDepth_) {
      if (keyIs(key, len, "name"))
        lastKey_ = LastKey::NAME;
      else if (keyIs(key, len, "description"))
        lastKey_ = LastKey::DESCRIPTION;
      else if (keyIs(key, len, "styles"))
        lastKey_ = LastKey::STYLES;
      else if (keyIs(key, len, "files"))
        lastKey_ = LastKey::FILES;
    } else if (rootSeen_ && depth_ == 1) {
      if (keyIs(key, len, "version"))
        lastKey_ = LastKey::VERSION;
      else if (keyIs(key, len, "baseUrl"))
        lastKey_ = LastKey::BASE_URL;
      else if (keyIs(key, len, "families"))
        lastKey_ = LastKey::FAMILIES;
    }
  }

  void onString(const char* value, size_t len) {
    switch (lastKey_) {
      case LastKey::BASE_URL:
        outBaseUrl_.assign(value, len);
        break;
      case LastKey::NAME:
        currentFamily_.name.assign(value, len);
        break;
      case LastKey::DESCRIPTION:
        currentFamily_.description.assign(value, len);
        break;
      case LastKey::FILE_NAME:
        currentFile_.name.assign(value, len);
        break;
      default:
        if (inStyles_ && depth_ == stylesDepth_) {
          currentFamily_.styles.emplace_back(value, len);
        }
        break;
    }
    lastKey_ = LastKey::NONE;
  }

  void onNumber(const char* value, size_t /*len*/) {
    // Parser tokens are null-terminated (same contract the BookFusion
    // streams rely on).
    if (lastKey_ == LastKey::VERSION) {
      version_ = static_cast<int>(strtol(value, nullptr, 10));
    } else if (lastKey_ == LastKey::FILE_SIZE) {
      currentFile_.size = static_cast<size_t>(strtoul(value, nullptr, 10));
    }
    lastKey_ = LastKey::NONE;
  }

  void onObjectStart() {
    if (depth_ == 0) {
      rootSeen_ = true;
    } else if (inFamilies_ && !inFamily_ && depth_ == familiesDepth_) {
      currentFamily_ = FontDownloadActivity::ManifestFamily{};
      familyDepth_ = depth_ + 1;
      inFamily_ = true;
    } else if (inFiles_ && !inFile_ && depth_ == filesDepth_) {
      currentFile_ = FontDownloadActivity::ManifestFile{};
      fileDepth_ = depth_ + 1;
      inFile_ = true;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onObjectEnd() {
    if (inFile_ && depth_ == fileDepth_) {
      currentFamily_.totalSize += currentFile_.size;
      currentFamily_.files.push_back(std::move(currentFile_));
      inFile_ = false;
      fileDepth_ = 0;
    } else if (inFamily_ && depth_ == familyDepth_) {
      outFamilies_.push_back(std::move(currentFamily_));
      inFamily_ = false;
      familyDepth_ = 0;
    }
    if (depth_ > 0) --depth_;
    if (rootSeen_ && depth_ == 0) rootClosed_ = true;
    lastKey_ = LastKey::NONE;
  }

  void onArrayStart() {
    if (lastKey_ == LastKey::FAMILIES && depth_ == 1) {
      inFamilies_ = true;
      familiesDepth_ = depth_ + 1;
    } else if (inFamily_ && lastKey_ == LastKey::STYLES && depth_ == familyDepth_) {
      inStyles_ = true;
      stylesDepth_ = depth_ + 1;
    } else if (inFamily_ && lastKey_ == LastKey::FILES && depth_ == familyDepth_) {
      inFiles_ = true;
      filesDepth_ = depth_ + 1;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onArrayEnd() {
    if (inStyles_ && depth_ == stylesDepth_) {
      inStyles_ = false;
      stylesDepth_ = 0;
    } else if (inFiles_ && depth_ == filesDepth_) {
      inFiles_ = false;
      filesDepth_ = 0;
    } else if (inFamilies_ && depth_ == familiesDepth_) {
      inFamilies_ = false;
      familiesDepth_ = 0;
    }
    if (depth_ > 0) --depth_;
    lastKey_ = LastKey::NONE;
  }

  std::vector<FontDownloadActivity::ManifestFamily>& outFamilies_;
  std::string& outBaseUrl_;
  StreamingJsonParser parser_;
  FontDownloadActivity::ManifestFamily currentFamily_;
  FontDownloadActivity::ManifestFile currentFile_;
  LastKey lastKey_ = LastKey::NONE;
  size_t bytesRead_ = 0;
  int version_ = 0;
  uint8_t depth_ = 0;
  uint8_t familiesDepth_ = 0;
  uint8_t familyDepth_ = 0;
  uint8_t stylesDepth_ = 0;
  uint8_t filesDepth_ = 0;
  uint8_t fileDepth_ = 0;
  bool rootSeen_ = false;
  bool rootClosed_ = false;
  bool inFamilies_ = false;
  bool inFamily_ = false;
  bool inStyles_ = false;
  bool inFiles_ = false;
  bool inFile_ = false;
};
}  // namespace

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  // Bracket the heap mystery: this activity has entered transfers at
  // 13-15KB free / ~2KB largest (vs ~45KB in comparable flows) with no
  // evidence of Bluetooth being up. Entry + post-shed + pre-transfer logs
  // triangulate who holds it.
  LOG_INF("FONT", "Heap at entry: free=%u largest=%u", ESP.getFreeHeap(),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  // This activity enters TLS with the tightest heap of any flow (~27KB free
  // observed): shed the cached font glyphs first — they re-warm on the next
  // paint — or the handshake's C++-side allocations can abort() the firmware.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCachesDeep();
  }
  LOG_DBG("FONT", "Heap after cache shed: free=%u largest=%u", ESP.getFreeHeap(),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));

  // Two SERIAL phases so the heap never carries both costs at once (streaming
  // the parse during the live TLS session ran the heap dry building families_
  // — ~15KB of manifest strings — and aborted at a vector growth; observed
  // on-device at 29KB free): (1) download the raw manifest to SD inside the
  // borrow, (2) after the connection is closed, stream-parse the file through
  // the SAX parser (see FontManifestJsonStream) on a quiet heap. No DOM ever.
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";
  auto dlResult = HttpDownloader::HTTP_ERROR;
  {
    // NOTE: the GitHub release URL 302s to a CDN, so TWO handshakes run
    // inside this borrow.
    TlsFramebufferBorrow borrow(renderer);
    dlResult = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
  }
  if (dlResult != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = "Failed to fetch font list";
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  FsFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Failed to read font list";
    return false;
  }

  FontManifestJsonStream stream(families_, baseUrl_);
  families_.reserve(16);
  uint8_t chunk[256];
  int got;
  while ((got = manifestFile.read(chunk, sizeof(chunk))) > 0) {
    stream.write(chunk, static_cast<size_t>(got));
  }
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);

  if (!stream.ok()) {
    LOG_ERR("FONT", "Manifest streaming parse error after %zu bytes", stream.bytesRead());
    errorMessage_ = "Invalid font manifest";
    return false;
  }
  if (stream.version() != FONTS_MANIFEST_VERSION) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", stream.version());
    errorMessage_ = "Unsupported manifest version";
    return false;
  }

  // Installed/update detection runs after the fetch (SD probing has no place
  // inside the borrow). Detect updates by comparing manifest file sizes with
  // files on disk — not a checksum, but a size mismatch reliably indicates a
  // rebuild in practice.
  for (auto& family : families_) {
    family.installed = fontInstaller_.isFamilyInstalled(family.name.c_str());
    if (!family.installed) continue;
    for (const auto& file : family.files) {
      char path[128];
      FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), path, sizeof(path));
      FsFile f;
      if (Storage.openFileForRead("FONT", path, f)) {
        size_t actual = f.fileSize();
        f.close();
        if (actual != file.size) {
          family.hasUpdate = true;
          break;
        }
      } else {
        // File missing on disk but family dir exists — treat as update
        family.hasUpdate = true;
        break;
      }
    }
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families", families_.size());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed && !families_[i].hasUpdate) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

size_t FontDownloadActivity::totalUninstalledSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed || f.hasUpdate) total += f.totalSize;
  }
  return total;
}

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    currentFileIndex_ = 0;
    currentFileTotal_ = family.files.size();
  }
  requestUpdateAndWait();

  if (!fontInstaller_.ensureFamilyDir(family.name.c_str())) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font directory";
    return;
  }

  // Keep the radio awake for the whole family: WiFi power-save on an idle
  // link (the user just sat on the family list) surfaces as a TCP connect
  // failure on the first request (observed on-device). Restored on every
  // return path.
  esp_wifi_set_ps(WIFI_PS_NONE);
  struct PsRestore {
    ~PsRestore() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
  } psRestore;

  for (size_t i = 0; i < family.files.size(); i++) {
    const auto& file = family.files[i];

    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
    }
    // Paint the per-file "Downloading Family (i/n)" frame and wait for it —
    // the screen freezes on it while the framebuffer is lent to this file's
    // TLS session below. The file counter still advances between files, so
    // the user sees progression without a live bar.
    requestUpdateAndWait();

    // Browsing the family list re-warmed the glyph cache since the
    // pre-manifest clear (observed on-device: 27KB free at manifest time
    // collapsed to 11KB free / 2KB largest here, starving even TCP connect).
    // The frame above is already painted — shed the glyphs again so the
    // transfer gets the heap; they re-warm on the next paint.
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCachesDeep();
    }
    LOG_DBG("FONT", "Heap after cache shed: free=%u largest=%u", ESP.getFreeHeap(),
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));

    char destPath[128];
    FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), destPath, sizeof(destPath));

    std::string url = baseUrl_ + file.name;

    // Retried like every other transfer: transient TCP-connect/DNS failures
    // and CDN drops self-heal instead of failing the whole family.
    auto result = HttpDownloader::HTTP_ERROR;
    constexpr int kMaxFileAttempts = 3;
    for (int attempt = 1; attempt <= kMaxFileAttempts; attempt++) {
      TlsFramebufferBorrow borrow(renderer);
      size_t lastLoggedMB = 0;
      result = HttpDownloader::downloadToFile(url, destPath, [&lastLoggedMB](size_t downloaded, size_t total) {
        const size_t mb = downloaded >> 20;
        if (mb > lastLoggedMB) {
          lastLoggedMB = mb;
          LOG_DBG("FONT", "Download progress: %u MB", (unsigned)mb);
          (void)total;
        }
      });
      if (result == HttpDownloader::OK) break;
      LOG_ERR("FONT", "Download attempt %d/%d failed: %s (%d)", attempt, kMaxFileAttempts, file.name.c_str(), result);
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Download failed: " + file.name;
      return;
    }

    if (!fontInstaller_.validateCpfontFile(destPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", destPath);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Invalid font file: " + file.name;
      return;
    }
  }

  fontInstaller_.refreshRegistry();
  family.installed = true;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

// --- Input handling ---

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect FontDownloadActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void FontDownloadActivity::activateSelected() {
  if (isDownloadAllSelected()) {
    downloadAll();
  } else {
    const auto& family = families_[familyIndexFromList(selectedIndex_)];
    if (!family.installed || family.hasUpdate) {
      downloadFamily(families_[familyIndexFromList(selectedIndex_)]);
    }
  }
  requestUpdateAndWait();
}

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    if (!families_.empty()) {
      int tappedIndex;
      switch (TouchListNav::tapRow(mappedInput, listRect(), listItemCount(), selectedIndex_,
                                   /*hasSubtitle=*/false, tappedIndex)) {
        case TouchListNav::TapResult::SelectionMoved:
          selectedIndex_ = tappedIndex;
          requestUpdate();
          return;
        case TouchListNav::TapResult::Activated:
          activateSelected();
          return;
        case TouchListNav::TapResult::None:
          break;
      }

      // Full Touch: a vertical swipe turns a page, matching the held side key.
      const int pageItems = GUI.listGeometry(listRect(), selectedIndex_, /*hasSubtitle=*/false).pageItems;
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

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!families_.empty()) {
        activateSelected();
        return;
      }
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        downloadFamily(families_[downloadingFamilyIndex_]);
        requestUpdateAndWait();
        return;
      } else {
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
        }
        requestUpdate();
      }
    }
  }
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
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

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_DOWNLOAD));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    if (families_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer, listRect(), listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (index == 0) {
              return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalUninstalledSize()) + ")";
            }
            return families_[familyIndexFromList(index)].name;
          },
          nullptr, nullptr,
          [this](int index) -> std::string {
            if (index == 0) return "";
            const auto& f = families_[familyIndexFromList(index)];
            if (f.hasUpdate) return tr(STR_UPDATE);
            if (f.installed) return tr(STR_INSTALLED);
            return f.description;
          },
          true,
          [this](int index) -> bool {
            if (index == 0) return false;
            const auto& f = families_[familyIndexFromList(index)];
            // Dim installed fonts, but not those with updates available
            return f.installed && !f.hasUpdate;
          });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + family.name + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    // Static frame — the framebuffer is lent to the transfer's TLS session, so
    // no live bar; the (i/n) counter above advances between files.
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, tr(STR_DOWNLOAD_WAIT));
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
