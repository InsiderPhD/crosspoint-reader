#include "DownloadUpdateFromUrlActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FirmwareFlasher.h"
#include "network/HttpDownloader.h"

namespace {
// Same SD temp slot the GitHub OTA install uses; removed on exit / after flash.
constexpr char kTmpPath[] = "/firmware_ota.bin";
}  // namespace

void DownloadUpdateFromUrlActivity::onEnter() {
  Activity::onEnter();
  state = State::ENTERING_URL;
  launchUrlEntry();
}

void DownloadUpdateFromUrlActivity::onExit() {
  // Drop WiFi and clean up the temp download if we bailed mid-flow.
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  Storage.remove(kTmpPath);
  Activity::onExit();
}

void DownloadUpdateFromUrlActivity::launchUrlEntry() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, std::string(tr(STR_ENTER_FIRMWARE_URL)),
                                              /*initialText=*/"https://", /*maxLength=*/0, InputType::Url),
      [this](const ActivityResult& result) { onUrlEntered(result); });
}

void DownloadUpdateFromUrlActivity::onUrlEntered(const ActivityResult& result) {
  if (result.isCancelled) {
    finish();
    return;
  }
  const auto* kb = std::get_if<KeyboardResult>(&result.data);
  if (!kb || kb->text.empty() || kb->text == "https://" || kb->text == "http://") {
    finish();
    return;
  }
  url = kb->text;
  LOG_DBG("FWURL", "URL entered: %s", url.c_str());

  {
    RenderLock lock(*this);
    state = State::CONNECTING_WIFI;
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& res) { onWifiSelectionComplete(!res.isCancelled); });
}

void DownloadUpdateFromUrlActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("FWURL", "WiFi connection failed");
    errorMessage = tr(STR_UPDATE_FAILED);
    RenderLock lock(*this);
    state = State::FAILED;
    return;
  }
  performDownloadAndFlash();
}

void DownloadUpdateFromUrlActivity::performDownloadAndFlash() {
  // Phase 1: download to SD.
  {
    RenderLock lock(*this);
    state = State::DOWNLOADING;
    downloadedBytes = 0;
    totalBytes = 0;
    lastRenderedPercent = 101;
  }
  requestUpdateAndWait();

  const auto dlResult = HttpDownloader::downloadToFile(
      url, kTmpPath,
      [this](size_t downloaded, size_t total) {
        downloadedBytes = downloaded;
        totalBytes = total;
        // immediate=true: we're in a blocking sync loop, so wake the render task directly.
        requestUpdate(true);
      },
      /*allowConfiguredAuth=*/false);

  if (dlResult != HttpDownloader::OK) {
    LOG_ERR("FWURL", "Download failed: %d", dlResult);
    Storage.remove(kTmpPath);
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  // Phase 2: raw-write the OTA partition (bypasses esp_image_verify — see OtaBootSwitch.h).
  {
    RenderLock lock(*this);
    state = State::FLASHING;
    writtenBytes = 0;
    lastRenderedPercent = 101;
  }
  requestUpdateAndWait();

  const auto flashResult = firmware_flash::flashFromSdPath(
      kTmpPath,
      +[](size_t written, size_t total, void* ctx) {
        auto* self = static_cast<DownloadUpdateFromUrlActivity*>(ctx);
        self->writtenBytes = written;
        self->totalBytes = total;
        self->requestUpdate(true);
      },
      this);

  Storage.remove(kTmpPath);

  if (flashResult != firmware_flash::Result::OK) {
    LOG_ERR("FWURL", "Flash failed: %s", firmware_flash::resultName(flashResult));
    errorMessage = tr(STR_FIRMWARE_WRITE_FAILED);
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  LOG_INF("FWURL", "URL firmware update complete, restarting");
  {
    RenderLock lock(*this);
    state = State::SUCCESS;
  }
  requestUpdateAndWait();
  delay(1500);
  ESP.restart();
}

void DownloadUpdateFromUrlActivity::loop() {
  if (state == State::FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

void DownloadUpdateFromUrlActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DOWNLOAD_FROM_URL));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  auto drawProgress = [&](const char* label, size_t done, size_t total) {
    const unsigned int pct = total > 0 ? static_cast<unsigned int>((done * 100) / total) : 0;
    if (pct == lastRenderedPercent) return false;
    lastRenderedPercent = pct;
    renderer.drawCenteredText(UI_10_FONT_ID, top, label, true, EpdFontFamily::BOLD);
    int y = top + lineHeight + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(pct), 100);
    return true;
  };

  if (state == State::CONNECTING_WIFI) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CONNECTING));
  } else if (state == State::DOWNLOADING) {
    if (!drawProgress(tr(STR_DOWNLOADING), downloadedBytes, totalBytes)) return;
  } else if (state == State::FLASHING) {
    if (!drawProgress(tr(STR_UPDATING), writtenBytes, totalBytes)) return;
    const int y = top + lineHeight + metrics.verticalSpacing + metrics.progressBarHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
  } else if (state == State::SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, tr(STR_RESTARTING_HINT));
  } else if (state == State::FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, errorMessage.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  // ENTERING_URL: the keyboard sub-activity is on top; nothing to draw.

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
