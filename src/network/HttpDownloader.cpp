#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <Memory.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <base64.h>
#include <esp_heap_caps.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  // Use NetworkClientSecure for HTTPS, regular NetworkClient for HTTP.
  // Own each client as its CONCRETE type. NetworkClient's destructor is
  // non-virtual, so holding a NetworkClientSecure through a
  // unique_ptr<NetworkClient> slices it on delete: ~NetworkClientSecure never
  // runs, its sslclient shared_ptr (whose custom deleter calls stop_ssl_socket)
  // is never released, and the entire mbedTLS session context (tens of KB)
  // leaks on every HTTPS transfer. On the no-PSRAM C3 a couple of leaks exhaust
  // the largest contiguous block and the next TLS handshake fails.
  std::unique_ptr<NetworkClientSecure> secureClient;
  std::unique_ptr<NetworkClient> plainClient;
  NetworkClient* client = nullptr;
  if (UrlUtils::isHttpsUrl(url)) {
    secureClient = std::make_unique<NetworkClientSecure>();
    secureClient->setInsecure();
    client = secureClient.get();
  } else {
    plainClient = std::make_unique<NetworkClient>();
    client = plainClient.get();
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // Use standard browser user agent for BookFusion compatibility
  bool isBookFusion = url.find("bookfusion.com") != std::string::npos;
  if (isBookFusion) {
    http.addHeader("User-Agent", "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36");
    http.addHeader("Accept", "*/*");
    http.addHeader("Referer", "https://www.bookfusion.com/");
  } else {
    http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  }

  // Add Basic HTTP auth if credentials are configured (but not for BookFusion URLs which use token auth)
  if (!isBookFusion && strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  http.writeToStream(&outContent);

  http.end();

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
                                                             ProgressCallback progress, bool allowConfiguredAuth) {
  // Use NetworkClientSecure for HTTPS, regular NetworkClient for HTTP.
  // Own each client as its CONCRETE type. NetworkClient's destructor is
  // non-virtual, so holding a NetworkClientSecure through a
  // unique_ptr<NetworkClient> slices it on delete: ~NetworkClientSecure never
  // runs, its sslclient shared_ptr (whose custom deleter calls stop_ssl_socket)
  // is never released, and the entire mbedTLS session context (tens of KB)
  // leaks on every HTTPS transfer. On the no-PSRAM C3 a couple of leaks exhaust
  // the largest contiguous block and the next TLS handshake fails.
  std::unique_ptr<NetworkClientSecure> secureClient;
  std::unique_ptr<NetworkClient> plainClient;
  NetworkClient* client = nullptr;
  if (UrlUtils::isHttpsUrl(url)) {
    secureClient = std::make_unique<NetworkClientSecure>();
    secureClient->setInsecure();
    client = secureClient.get();
  } else {
    plainClient = std::make_unique<NetworkClient>();
    client = plainClient.get();
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // We stream the body ourselves (see below) instead of HTTPClient::writeToStream
  // to avoid its internal 4 KB malloc. For chunked transfers we must decode the
  // chunk framing, so ask HTTPClient to retain the Transfer-Encoding header.
  static const char* kCollectedHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(kCollectedHeaders, 1);

  // Use standard browser user agent for BookFusion compatibility
  bool isBookFusion = url.find("bookfusion.com") != std::string::npos;
  if (isBookFusion) {
    http.addHeader("User-Agent", "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36");
    http.addHeader("Accept", "*/*");
    http.addHeader("Referer", "https://www.bookfusion.com/");
  } else {
    http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  }

  // Add Basic HTTP auth if credentials are configured (but not for BookFusion URLs which use token auth)
  if (allowConfiguredAuth && !isBookFusion && strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    http.end();
    return HTTP_ERROR;
  }

  const int64_t reportedLength = http.getSize();
  const size_t contentLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  // Heap snapshot before the transfer. The failure we're guarding against
  // (HTTPC_ERROR_TOO_LESS_RAM, -8) is a *largest-free-block* shortage, not a
  // total-free shortage, so log both.
  LOG_DBG("HTTP", "Heap before transfer: free=%u largest=%u", ESP.getFreeHeap(),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));

  size_t downloaded = 0;
  bool writeOk = true;

  // Stream the body through a single small buffer rather than
  // HTTPClient::writeToStream(), which mallocs HTTP_TCP_RX_BUFFER_SIZE (4096)
  // bytes internally for its read buffer. On the no-PSRAM C3 this activity runs
  // at only ~16 KB free with the largest contiguous block often under 8 KB, so
  // that 4 KB request fails mid-transfer with HTTPC_ERROR_TOO_LESS_RAM (-8) —
  // the error seen on BookFusion downloads. A 1 KB block is reliably available
  // even when the heap is fragmented. Allocated once (too big for the small
  // task stack) and reused for the whole transfer.
  constexpr size_t kRxChunk = 1024;
  auto rx = makeUniqueNoThrow<uint8_t[]>(kRxChunk);
  NetworkClient* stream = http.getStreamPtr();
  if (!rx || !stream) {
    LOG_ERR("HTTP", "RX setup failed (buf=%p stream=%p)", static_cast<void*>(rx.get()),
            static_cast<void*>(stream));
    file.close();
    http.end();
    Storage.remove(destPath.c_str());
    return rx ? HTTP_ERROR : FILE_ERROR;
  }

  // Drain `count` body bytes from the socket into the file via the 1 KB buffer.
  // count < 0 streams until the peer closes the connection. Plain closure (not
  // std::function) so there's no heap/closure overhead. Returns false only on a
  // short SD write; sets writeOk/downloaded.
  const auto drainToFile = [&](int64_t count) -> bool {
    while (http.connected() && count != 0) {
      size_t want = kRxChunk;
      if (count > 0 && count < static_cast<int64_t>(want)) {
        want = static_cast<size_t>(count);
      }
      const int got = stream->readBytes(rx.get(), want);
      if (got <= 0) {
        // readBytes() timed out with the socket still open; retry. connected()
        // dropping (peer closed) is what ends a length-unknown transfer.
        delay(1);
        continue;
      }
      const size_t wrote = file.write(rx.get(), static_cast<size_t>(got));
      downloaded += wrote;
      if (wrote != static_cast<size_t>(got)) {
        writeOk = false;
        return false;
      }
      if (count > 0) {
        count -= got;
      }
      if (progress) {
        progress(downloaded, contentLength);
      }
      delay(0);  // yield to other tasks / feed the watchdog
    }
    return true;
  };

  String transferEncoding = http.header("Transfer-Encoding");
  transferEncoding.toLowerCase();
  const bool chunked = transferEncoding.indexOf("chunked") >= 0;

  // Set true only when we reach a clean end-of-body. A chunked transfer that
  // stops without its terminating zero-length chunk is a truncation and must be
  // rejected — otherwise a partial JPEG is written and reported as success, then
  // fails to decode mid-stream (the corrupt-cover bug). The contentLength>0 path
  // is validated separately by the size check below.
  bool transferComplete = false;

  if (contentLength > 0) {
    // Identity transfer with a known length — read exactly that many bytes.
    drainToFile(static_cast<int64_t>(contentLength));
    transferComplete = true;  // completeness enforced by the size check below
  } else if (chunked) {
    // Decode HTTP chunked framing ourselves (mirrors writeToStream()'s chunked
    // branch) so an individual >=4 KB chunk never reaches the framework's 4 KB
    // malloc. Each chunk is a hex length line, the data, then a trailing CRLF;
    // a literal "0" length chunk terminates the body.
    //
    // An EMPTY header line is a read timeout, NOT a terminator: strtol("") is 0,
    // so treating chunkLen<=0 as end-of-body would silently truncate the file on
    // any transient stall. Retry a bounded number of times and only finish
    // cleanly on a real zero-length chunk.
    constexpr int kMaxConsecutiveStalls = 10;  // ~10s at the 1s stream timeout
    int stalls = 0;
    while (http.connected()) {
      String chunkHeader = stream->readStringUntil('\n');
      chunkHeader.trim();  // drop the trailing \r
      if (chunkHeader.isEmpty()) {
        if (++stalls > kMaxConsecutiveStalls) {
          break;  // dead/half-open connection — leaves transferComplete false
        }
        delay(1);
        continue;
      }
      stalls = 0;
      const long chunkLen = strtol(chunkHeader.c_str(), nullptr, 16);
      if (chunkLen <= 0) {
        transferComplete = true;  // terminating 0-length chunk
        break;
      }
      if (!drainToFile(chunkLen)) {
        break;  // short SD write — leaves transferComplete false
      }
      uint8_t crlf[2];
      stream->readBytes(crlf, sizeof(crlf));  // consume the chunk's trailing CRLF
    }
  } else {
    // Identity with no length — stream until the peer closes the connection.
    // A premature close is indistinguishable from a clean one here, so this path
    // cannot detect truncation; sources needing integrity should send a
    // Content-Length or use chunked encoding.
    drainToFile(-1);
    transferComplete = true;
  }

  file.close();
  http.end();

  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Guard against partial writes even if the transfer reported success.
  if (!writeOk) {
    LOG_ERR("HTTP", "Write failed during download");
    Storage.remove(destPath.c_str());
    return FILE_ERROR;
  }

  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  // A chunked transfer that never reached its terminating zero-length chunk is
  // truncated. Reject it loudly rather than handing a partial file downstream.
  if (!transferComplete) {
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

  return OK;
}
