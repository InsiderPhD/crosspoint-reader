#pragma once

#include <cstddef>
#include <string>

#include "activities/Activity.h"

/**
 * Backup firmware update path: download a firmware.bin from a user-entered URL and flash it.
 *
 * Flow:
 *  1) onEnter -> KeyboardEntryActivity (URL input) to capture the URL.
 *  2) WifiSelectionActivity to get online.
 *  3) HttpDownloader::downloadToFile() streams the .bin to an SD temp file.
 *  4) firmware_flash::flashFromSdPath() raw-writes the OTA partition + switches otadata, then
 *     ESP.restart(). The raw-write path bypasses esp_image_verify, which rejects valid images
 *     on X3/X4 units (see OtaBootSwitch.h) — the same reason network OTA needs this path.
 *
 * This is deliberately host-agnostic (HTTP or HTTPS, no cert pinning) so it works against a
 * self-hosted/LAN recovery server when GitHub OTA is unavailable.
 */
class DownloadUpdateFromUrlActivity : public Activity {
 public:
  enum class State {
    ENTERING_URL,
    CONNECTING_WIFI,
    DOWNLOADING,
    FLASHING,
    SUCCESS,
    FAILED,
  };

  explicit DownloadUpdateFromUrlActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DownloadUpdateFromUrl", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::DOWNLOADING || state == State::FLASHING; }
  bool skipLoopDelay() override { return state == State::DOWNLOADING || state == State::FLASHING; }

 private:
  State state = State::ENTERING_URL;

  std::string url;
  std::string errorMessage;
  size_t downloadedBytes = 0;
  size_t totalBytes = 0;
  size_t writtenBytes = 0;
  unsigned int lastRenderedPercent = 101;

  void launchUrlEntry();
  void onUrlEntered(const ActivityResult& result);
  void onWifiSelectionComplete(bool success);
  void performDownloadAndFlash();
};
