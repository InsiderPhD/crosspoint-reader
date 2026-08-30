#include "OtaUpdater.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <SecureHttpClient.h>
#include <esp_wifi.h>

#include "CrossPointSettings.h"
#include "FirmwareFlasher.h"
#include "HttpDownloader.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/InsiderPhD/crosspoint-reader/releases/latest";

// Pre-release channel (Dev Mode -> "Enable Pre-Releases"). /releases/latest
// deliberately skips prereleases -- that is exactly how stable devices are kept
// off beta/rc/x4pro builds -- so the opt-in channel reads the release LIST and
// takes the newest entry carrying this build's asset. per_page bounds the
// response: the body streams through the parser and is never buffered, but each
// release carries its full release notes, so ask for a window rather than the
// default 30.
constexpr char releaseListUrl[] = "https://api.github.com/repos/InsiderPhD/crosspoint-reader/releases?per_page=10";

// SD staging slot for the downloaded image, shared by downloadUpdate() and
// flashUpdate(). Reuses the SD recovery temp path; kept on SD root so a
// failed/interrupted flash leaves an obvious artifact rather than silently
// consuming hidden space.
constexpr char kTmpPath[] = "/firmware_ota.bin";

// One release carries a binary per MCU family and they are NOT interchangeable:
// FirmwareFlasher raw-writes the OTA partition without an image chip-ID check
// (that bypass is deliberate — X3/X4 units reject otherwise-valid images with
// bogus efuse-blk-rev errors), so a C3 image landing on an X4 Pro's ESP32-S3
// produces an unbootable device. Match only this build's own asset name; a
// release with no matching asset reports NO_UPDATE, which is the safe outcome.
#if FREEINK_DEVICE_X4PRO
constexpr char kFirmwareAssetName[] = "x4pro_firmware.bin";
#else
constexpr char kFirmwareAssetName[] = "firmware.bin";
#endif
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  // Gated on devMode as well as the toggle itself: the toggle only exists on the
  // Dev tab, so leaving Dev Mode must not strand a device on the beta channel
  // with no visible switch. Turning Dev Mode back on restores the choice.
  const bool preReleases = SETTINGS.devMode != 0 && SETTINGS.allowPreReleases != 0;
  const char* releaseUrl = preReleases ? releaseListUrl : latestReleaseUrl;
  ReleaseJsonParser releaseParser(kFirmwareAssetName, /*releaseList=*/preReleases);

  LOG_DBG("OTA", "Checking for update (current: %s, channel: %s)", CROSSPOINT_VERSION,
          preReleases ? "pre-release" : "stable");

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
  if (!http.begin(releaseUrl)) {
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
    // An empty (or asset-less) release list is a normal outcome for the
    // pre-release channel, not a malformed payload -- report it as such so the
    // UI says "no update" instead of "update failed".
    if (preReleases) {
      LOG_DBG("OTA", "No release with a %s asset in the pre-release list", kFirmwareAssetName);
      return NO_UPDATE;
    }
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No %s asset found", kFirmwareAssetName);
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

OtaUpdater::OtaUpdaterError OtaUpdater::downloadUpdate() {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }
  if (otaUrl.empty()) {
    LOG_ERR("OTA", "No firmware URL to install");
    return INTERNAL_UPDATE_ERROR;
  }

  /* Disable WiFi power saving for a stable download. */
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Download firmware.bin to SD, quietly: the caller holds the framebuffer
  // lent to this transfer's TLS session, so nothing may render — progress is a
  // serial-only per-MB heartbeat. allowConfiguredAuth=false: never attach OPDS
  // Basic auth to a firmware fetch. Retried: a ~6MB transfer over a slow CDN
  // can drop mid-stream, and the GitHub asset URL is stable across attempts.
  totalSize = otaSize;
  auto dlResult = HttpDownloader::HTTP_ERROR;
  constexpr int kMaxDownloadAttempts = 3;
  for (int attempt = 1; attempt <= kMaxDownloadAttempts; attempt++) {
    processedSize = 0;
    size_t lastLoggedMB = 0;
    dlResult = HttpDownloader::downloadToFile(
        otaUrl, kTmpPath,
        [this, &lastLoggedMB](size_t downloaded, size_t total) {
          processedSize = downloaded;
          totalSize = total > 0 ? total : otaSize;
          const size_t mb = downloaded >> 20;
          if (mb > lastLoggedMB) {
            lastLoggedMB = mb;
            LOG_DBG("OTA", "Download progress: %u/%u MB", (unsigned)mb, (unsigned)(totalSize >> 20));
          }
        },
        /*allowConfiguredAuth=*/false, /*expectedSize=*/otaSize);

    if (dlResult == HttpDownloader::OK) break;
    LOG_ERR("OTA", "Firmware download attempt %d/%d failed: %d", attempt, kMaxDownloadAttempts, dlResult);
  }

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (dlResult != HttpDownloader::OK) {
    Storage.remove(kTmpPath);
    return HTTP_ERROR;
  }
  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::flashUpdate(ProgressCallback onProgress, void* ctx) {
  // X3/X4 units reject otherwise-valid images via esp_image_verify (bogus efuse-blk-rev),
  // so the streaming esp_https_ota path fails at finish on those devices. Instead raw-write
  // the OTA partition + switch otadata from the staged SD image, which bypasses the runtime
  // verify (same scheme as SD recovery and the web flasher — see OtaBootSwitch.h).
  // flashFromSdPath re-validates the image (header / segment table / XOR / SHA trailer)
  // before writing.
  uiProgressCb = onProgress;
  uiProgressCtx = ctx;

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
