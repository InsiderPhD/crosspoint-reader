#pragma once

#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();

  // The install is split so the caller can lend the framebuffer to the
  // download's TLS session (TlsFramebufferBorrow) while a pre-painted static
  // frame holds the panel — the ~6MB transfer drops mid-stream on this heap
  // otherwise, exactly like the BookFusion book download did. downloadUpdate()
  // is quiet (serial-only per-MB heartbeat, retries) and stages the image on
  // SD; flashUpdate() raw-writes it with UI progress (no TLS, rendering is
  // safe again). Call downloadUpdate() first; flashUpdate() consumes the
  // staged file.
  OtaUpdaterError downloadUpdate();
  OtaUpdaterError flashUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);

 private:
  // UI progress callback + context, stashed during installUpdate() so the C-style
  // FirmwareFlasher progress trampoline (which only carries a void*) can forward ticks.
  ProgressCallback uiProgressCb = nullptr;
  void* uiProgressCtx = nullptr;
};
