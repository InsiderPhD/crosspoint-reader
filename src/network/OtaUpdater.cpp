#include "OtaUpdater.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <SecureHttpClient.h>
#include <esp_wifi.h>

#include "FirmwareFlasher.h"
#include "HttpDownloader.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/InsiderPhD/crosspoint-reader/releases/latest";
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  ReleaseJsonParser releaseParser;

  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Transport is freeink::SecureHttpClient (wolfSSL), the last esp-tls call
  // site to migrate. setInsecure() is not a downgrade: the previous esp-tls
  // config set skip_cert_common_name_check, which accepted any CA-signed cert
  // for any hostname — and the firmware binary itself already downloads via
  // HttpDownloader (insecure), with FirmwareFlasher re-validating the image
  // (header / segment table / XOR / SHA trailer) before writing. Real
  // supply-chain protection would be signed releases verified in the flasher,
  // not TLS pinning here.
  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(15000);
  // GitHub's API rejects UA-less requests.
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!http.begin(latestReleaseUrl)) {
    LOG_ERR("OTA", "Bad release URL");
    return INTERNAL_UPDATE_ERROR;
  }

  // The release JSON (tens of KB with the release notes) streams straight
  // through the parser — never buffered whole.
  size_t totalBytesReceived = 0;
  const int httpCode = http.GET([&](const uint8_t* data, size_t len) {
    totalBytesReceived += len;
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });

  if (httpCode < 0) {
    LOG_ERR("OTA", "Release check request failed: %d", httpCode);
    return HTTP_ERROR;
  }
  if (httpCode != 200) {
    LOG_ERR("OTA", "Release check HTTP %d", httpCode);
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);
  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  const auto currentVersion = CROSSPOINT_VERSION;

  // Parse "major.minor.patch", tolerating a leading "v" (and any other leading non-digits) and
  // ignoring trailing suffixes like "-rc+hash" / "-slim" / "-<branch>". Release tags omit the
  // "v" (e.g. "1.5.3") while CROSSPOINT_VERSION carries it ("v1.5.3"); sscanf on the "v" form
  // fails and, with uninitialised locals, made this comparison non-deterministic. Always zero
  // the outputs and skip to the first digit so both forms parse the same way.
  auto parseSemver = [](const char* s, int& major, int& minor, int& patch) {
    major = minor = patch = 0;
    while (*s && (*s < '0' || *s > '9')) ++s;
    sscanf(s, "%d.%d.%d", &major, &minor, &patch);
  };

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;
  parseSemver(latestVersion.c_str(), latestMajor, latestMinor, latestPatch);
  parseSemver(currentVersion, currentMajor, currentMinor, currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }
  if (otaUrl.empty()) {
    LOG_ERR("OTA", "No firmware URL to install");
    return INTERNAL_UPDATE_ERROR;
  }

  // X3/X4 units reject otherwise-valid images via esp_image_verify (bogus efuse-blk-rev),
  // so the streaming esp_https_ota path fails at finish on those devices. Instead download
  // the .bin to SD, then raw-write the OTA partition + switch otadata, which bypasses the
  // runtime verify (same scheme as SD recovery and the web flasher — see OtaBootSwitch.h).
  uiProgressCb = onProgress;
  uiProgressCtx = ctx;

  // Reuse the SD recovery temp slot. Kept on SD root so a failed/interrupted flash leaves an
  // obvious artifact rather than silently consuming hidden space.
  static constexpr char kTmpPath[] = "/firmware_ota.bin";

  /* Disable WiFi power saving for a stable download. */
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Phase 1: download firmware.bin to SD. allowConfiguredAuth=false: never attach OPDS Basic
  // auth to a firmware fetch.
  totalSize = otaSize;
  processedSize = 0;
  const auto dlResult = HttpDownloader::downloadToFile(
      otaUrl, kTmpPath,
      [this, onProgress, ctx](size_t downloaded, size_t total) {
        processedSize = downloaded;
        totalSize = total > 0 ? total : otaSize;
        if (onProgress) onProgress(ctx);
      },
      /*allowConfiguredAuth=*/false);

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (dlResult != HttpDownloader::OK) {
    LOG_ERR("OTA", "Firmware download failed: %d", dlResult);
    Storage.remove(kTmpPath);
    return HTTP_ERROR;
  }

  // Phase 2: raw-write + otadata switch. flashFromSdPath re-validates the image (header /
  // segment table / XOR / SHA trailer) before writing.
  processedSize = 0;
  const auto flashResult = firmware_flash::flashFromSdPath(
      kTmpPath,
      +[](size_t written, size_t total, void* c) {
        auto* self = static_cast<OtaUpdater*>(c);
        self->processedSize = written;
        self->totalSize = total;
        if (self->uiProgressCb) self->uiProgressCb(self->uiProgressCtx);
      },
      this);

  Storage.remove(kTmpPath);

  if (flashResult != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Raw flash failed: %s", firmware_flash::resultName(flashResult));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed (raw-write)");
  return OK;
}
