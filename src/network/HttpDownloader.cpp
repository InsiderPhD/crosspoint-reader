#include "HttpDownloader.h"

#include <Logging.h>
#include <SecureHttpClient.h>
#include <StreamString.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "CrossPointSettings.h"

// wolfSSL's Arduino port leaves its logging hook to the application
// (wolfcrypt/src/logging.c calls it whenever logging is enabled; the reference
// is unconditional, so the symbol must exist even in release builds). Route it
// through the project logger. Same approach as upstream's HttpDownloader.
extern "C" int wolfSSL_Arduino_Serial_Print(const char* const msg) {
  LOG_DBG("WOLFSSL", "%s", msg);
  return 0;
}

namespace {

// Shared request setup: trust model, redirects, UA and auth. Transport is
// freeink::SecureHttpClient (wolfSSL TLS 1.3) — it dispatches https/http
// internally, decodes chunked framing itself, and streams the body through a
// 2KB stack buffer, so no 16KB mbedTLS record pair and no 4KB
// HTTPClient::writeToStream malloc ever hits this heap. setInsecure() is the
// project's historical trust model (encrypted, server not authenticated).
void configureRequest(freeink::SecureHttpClient& http, const std::string& url, bool allowConfiguredAuth) {
  http.setInsecure();
  http.setFollowRedirects(5);  // BookFusion pre-signed URLs redirect to a CDN

  const bool isBookFusion = url.find("bookfusion.com") != std::string::npos;
  if (isBookFusion) {
    // Standard browser user agent for BookFusion compatibility
    http.setUserAgent("Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36");
    http.addHeader("Accept", "*/*");
    http.addHeader("Referer", "https://www.bookfusion.com/");
  } else {
    http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  }

  // Basic HTTP auth if credentials are configured (not for BookFusion URLs,
  // which use token auth)
  if (allowConfiguredAuth && !isBookFusion && strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    http.setBasicAuth(SETTINGS.opdsUsername, SETTINGS.opdsPassword);
  }
}

}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  freeink::SecureHttpClient http;
  configureRequest(http, url, true);

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  if (!http.begin(url)) {
    LOG_ERR("HTTP", "Fetch failed: bad URL");
    return false;
  }

  // getStatus() is valid inside the sink (headers parse before the body);
  // error bodies are drained without reaching the caller's stream.
  bool sinkError = false;
  const int httpCode = http.GET([&](const uint8_t* data, size_t len) {
    if (http.getStatus() != 200) return true;
    if (outContent.write(data, len) != len) {
      sinkError = true;
      return false;
    }
    return true;
  });

  if (httpCode != 200 || sinkError) {
    // -1 = transport (TCP connect / write / status line). Log where THIS
    // device sits on the network — a saved-credentials auto-join onto the
    // wrong SSID/subnet makes LAN servers unreachable and is otherwise
    // invisible in the logs.
    LOG_ERR("HTTP", "Fetch failed: %d%s (ip=%s, rssi=%d)", httpCode, sinkError ? " (sink error)" : "",
            WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return false;
  }

  LOG_DBG("HTTP", "Fetch success");
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent) {
  StreamString stream;
  if (!fetchUrl(url, stream)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool allowConfiguredAuth,
                                                             size_t expectedSize) {
  freeink::SecureHttpClient http;
  configureRequest(http, url, allowConfiguredAuth);

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  if (!http.begin(url)) {
    LOG_ERR("HTTP", "Download failed: bad URL");
    return HTTP_ERROR;
  }

  if (progress) {
    // SecureHttpClient reports (downloaded, total-from-Content-Length-or-0)
    // after each delivered chunk — the same shape our callers expect.
    http.setProgressCallback([&progress](size_t downloaded, size_t total) {
      progress(downloaded, total);
      return true;
    });
  }

  // Heap snapshot before the transfer: the historical failure mode here was a
  // *largest-free-block* shortage, not a total-free shortage, so log both.
  LOG_DBG("HTTP", "Heap before transfer: free=%u largest=%u", ESP.getFreeHeap(),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));

  // The file is created lazily on the first 200-status body chunk, so an HTTP
  // error (or a redirect chain that never resolves) leaves any existing file
  // at destPath untouched.
  FsFile file;
  bool fileOpen = false;
  bool fileError = false;
  size_t downloaded = 0;

  const int httpCode = http.GET([&](const uint8_t* data, size_t len) {
    if (http.getStatus() != 200) return true;  // drain error body
    if (!fileOpen) {
      if (Storage.exists(destPath.c_str())) {
        Storage.remove(destPath.c_str());
      }
      if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
        LOG_ERR("HTTP", "Failed to open file for writing");
        fileError = true;
        return false;
      }
      fileOpen = true;
    }
    const size_t wrote = file.write(data, len);
    downloaded += wrote;
    if (wrote != len) {
      fileError = true;
      return false;
    }
    delay(0);  // yield to other tasks / feed the watchdog
    return true;
  });

  if (fileOpen) {
    file.close();
  }

  if (fileError) {
    LOG_ERR("HTTP", "Write failed during download");
    if (fileOpen) {
      Storage.remove(destPath.c_str());
    }
    return FILE_ERROR;
  }

  if (httpCode != 200) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    return HTTP_ERROR;
  }

  const size_t contentLength = http.hasContentLength() ? http.getContentLength() : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    if (fileOpen) {
      Storage.remove(destPath.c_str());
    }
    return HTTP_ERROR;
  }

  // A body that stopped short of its framing (chunked terminator or
  // Content-Length) is a truncation. Reject it loudly rather than handing a
  // partial file downstream (the corrupt-cover bug).
  if (!http.responseComplete()) {
    LOG_ERR("HTTP", "Transfer truncated: got %zu bytes without a clean end-of-body", downloaded);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  // Cross-check against the size the caller knows independently (e.g. BookFusion's
  // API download_size). The Content-Length check above only fires when the server
  // sends that header; BookFusion's pre-signed URLs sometimes stream without one,
  // so a connection dropped mid-transfer yields a silently truncated EPUB that
  // every check above accepts. The expected size is an independent record of the
  // full file, so reject a download that came up well short of it. A tolerance
  // band (87.5%) absorbs the small, legitimate differences between the advertised
  // size and the bytes actually served while still catching the gross shortfall
  // of a truncated transfer.
  if (expectedSize > 0) {
    const size_t minAcceptable = expectedSize - expectedSize / 8;
    if (downloaded < minAcceptable) {
      LOG_ERR("HTTP", "Incomplete download: got %zu bytes, expected ~%zu", downloaded, expectedSize);
      Storage.remove(destPath.c_str());
      return HTTP_ERROR;
    }
  }

  return OK;
}
