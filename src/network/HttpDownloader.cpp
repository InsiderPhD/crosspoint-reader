#include "HttpDownloader.h"

#include <DevicePolicy.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <StreamString.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "DownloadFailureLog.h"

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

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  // begin() resets the per-request header list, so it must run BEFORE
  // configureRequest adds the BookFusion Accept/Referer headers.
  if (!http.begin(url)) {
    LOG_ERR("HTTP", "Fetch failed: bad URL");
    return false;
  }
  configureRequest(http, url, true);

  // getStatus() is valid inside the sink (headers parse before the body);
  // error bodies are drained without reaching the caller's stream.
  bool sinkError = false;
  const int httpCode = http.GET([&](const uint8_t* data, size_t len) {
    esp_task_wdt_reset();  // download length is network-bound; feed the loop WDT per chunk
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

namespace {
// Status of the last downloadToFile() response; see HttpDownloader::lastHttpStatus().
int gLastHttpStatus = 0;
}  // namespace

int HttpDownloader::lastHttpStatus() { return gLastHttpStatus; }

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool allowConfiguredAuth,
                                                             size_t expectedSize, const volatile bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             const std::vector<Header>& headers, bool resumePartial) {
  gLastHttpStatus = 0;
  freeink::SecureHttpClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  // begin() resets the per-request header list, so it must run BEFORE anything
  // below adds headers (previously the BookFusion Accept/Referer pair and a
  // plugin's own headers were added first and silently discarded here).
  if (!http.begin(url)) {
    LOG_ERR("HTTP", "Download failed: bad URL");
    return HTTP_ERROR;
  }
  configureRequest(http, url, allowConfiguredAuth);

  // Per-request credentials/headers (SD plugins). Set after configureRequest so
  // a plugin's own auth wins over anything the shared setup attached.
  if (!username.empty()) {
    http.setBasicAuth(username, password);
  }
  for (const auto& header : headers) {
    http.addHeader(header.first, header.second);
  }

  // Resume: bytes already on disk from an earlier interrupted transfer. A
  // partial that is already as large as the expected file cannot be a valid
  // prefix (the server would answer 416 anyway), so it is discarded instead.
  // `resumeBase` is what the progress/size maths add to this session's bytes;
  // it drops to 0 inside the sink if the server ignores the Range and sends a
  // full 200 body, which restarts the file from scratch.
  size_t resumeBase = 0;
  if (resumePartial && Storage.exists(destPath.c_str())) {
    {
      FsFile partial = Storage.open(destPath.c_str(), O_RDONLY);
      if (partial) {
        resumeBase = partial.fileSize();
        partial.close();
      }
    }
    if (expectedSize > 0 && resumeBase >= expectedSize) {
      LOG_ERR("HTTP", "Partial file (%u bytes) is not smaller than the expected %u; starting over",
              (unsigned)resumeBase, (unsigned)expectedSize);
      resumeBase = 0;
    }
    if (resumeBase > 0) {
      char range[40];
      snprintf(range, sizeof(range), "bytes=%u-", (unsigned)resumeBase);
      http.addHeader("Range", range);
      LOG_INF("HTTP", "Resuming transfer from byte %u", (unsigned)resumeBase);
    } else {
      Storage.remove(destPath.c_str());
    }
  }
  // For the failure log: what was asked for, separately from what the server
  // did about it (resumeBase itself drops to 0 when a 200 restarts the file).
  const size_t rangeRequested = resumeBase;

  if (progress) {
    // SecureHttpClient reports (downloaded, total-from-Content-Length-or-0)
    // after each delivered chunk — the same shape our callers expect, offset
    // by the resumed prefix so the bar never jumps backwards.
    http.setProgressCallback([&progress, &resumeBase](size_t downloaded, size_t total) {
      progress(resumeBase + downloaded, total > 0 ? resumeBase + total : 0);
      return true;
    });
  }

  // Heap snapshot before the transfer: the historical failure mode here was a
  // *largest-free-block* shortage, not a total-free shortage, so log both.
  const uint32_t heapFreeBefore = ESP.getFreeHeap();
  const uint32_t heapLargestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("HTTP", "Heap before transfer: free=%u largest=%u", (unsigned)heapFreeBefore, (unsigned)heapLargestBefore);

  // The file is created lazily on the first 200-status body chunk, so an HTTP
  // error (or a redirect chain that never resolves) leaves any existing file
  // at destPath untouched.
  FsFile file;
  bool fileOpen = false;
  bool fileError = false;
  size_t downloaded = 0;
  bool cancelled = false;
  // Transfer-shape counters for the failure log: a stall shows up as a large
  // gap between chunks; a slow link as a low chunk rate.
  const unsigned long requestMs = millis();
  unsigned long firstChunkMs = 0;
  unsigned long lastChunkMs = 0;
  unsigned long maxChunkGapMs = 0;
  uint32_t chunkCount = 0;

#if CROSSPOINT_DOWNLOAD_WRITE_BUFFER > 0
  // Write coalescing (roomy boards only — see CROSSPOINT_DOWNLOAD_WRITE_BUFFER).
  // The transport hands us the body in 2KB pieces, and each file.write() carries
  // the HalStorage mutex, SdFat's bookkeeping and (on SDMMC boards) a DMA-bounce
  // malloc+memcpy per transfer. Batching them into 32KB writes turns ~16 short
  // SDMMC transactions into one multi-sector run. One allocation for the whole
  // transfer, freed on return; on a PSRAM board it lands in PSRAM (>=4KB), so it
  // costs no internal DRAM. A failed allocation is not an error — the per-chunk
  // path below still runs. The whole block is compiled out on the C3, whose
  // heap has nothing to spare for it.
  std::unique_ptr<uint8_t[]> writeBuffer = makeUniqueNoThrow<uint8_t[]>(CROSSPOINT_DOWNLOAD_WRITE_BUFFER);
  size_t buffered = 0;
  if (!writeBuffer) {
    LOG_DBG("HTTP", "No %u-byte write buffer; writing chunks straight through",
            (unsigned)CROSSPOINT_DOWNLOAD_WRITE_BUFFER);
  }

  // Flushes whatever is buffered. Returns false on a short write (caller turns
  // that into FILE_ERROR); resets the fill either way so a failure can't be
  // re-flushed against a half-written file.
  const auto flushBuffer = [&]() {
    if (buffered == 0) return true;
    const size_t pending = buffered;
    buffered = 0;
    return file.write(writeBuffer.get(), pending) == pending;
  };

  const unsigned long startMs = millis();
#endif

  const int httpCode = http.GET([&](const uint8_t* data, size_t len) {
    esp_task_wdt_reset();  // download length is network-bound; feed the loop WDT per chunk
    const int status = http.getStatus();
    if (status != 200 && status != 206) return true;  // drain error body
    {
      const unsigned long now = millis();
      if (chunkCount == 0) {
        firstChunkMs = now;
      } else if (now - lastChunkMs > maxChunkGapMs) {
        maxChunkGapMs = now - lastChunkMs;
      }
      lastChunkMs = now;
      chunkCount++;
    }
    // Polled here rather than in the progress callback: the callback only fires
    // once a chunk has been delivered, and returning false from the sink is the
    // one path the transport already treats as "stop reading the body".
    if (cancelFlag && *cancelFlag) {
      cancelled = true;
      return false;
    }
    if (!fileOpen) {
      if (status == 206 && resumeBase > 0) {
        // Server honoured the Range: continue the existing file.
        file = Storage.open(destPath.c_str(), O_WRONLY | O_APPEND);
        if (!file) {
          LOG_ERR("HTTP", "Failed to reopen partial file for append");
          fileError = true;
          return false;
        }
      } else {
        if (resumeBase > 0) {
          LOG_INF("HTTP", "Server ignored the Range request (status %d); restarting from byte 0", status);
          resumeBase = 0;
        }
        if (Storage.exists(destPath.c_str())) {
          Storage.remove(destPath.c_str());
        }
        if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
          LOG_ERR("HTTP", "Failed to open file for writing");
          fileError = true;
          return false;
        }
      }
      fileOpen = true;
    }
#if CROSSPOINT_DOWNLOAD_WRITE_BUFFER > 0
    if (writeBuffer) {
      // Fill, flushing whenever the buffer is full. A chunk larger than the
      // buffer (never happens with a 2KB transport chunk, but the sink contract
      // doesn't promise a size) is handled by the loop, not assumed away.
      size_t offset = 0;
      while (offset < len) {
        const size_t space = CROSSPOINT_DOWNLOAD_WRITE_BUFFER - buffered;
        const size_t take = (len - offset) < space ? (len - offset) : space;
        memcpy(writeBuffer.get() + buffered, data + offset, take);
        buffered += take;
        offset += take;
        if (buffered == CROSSPOINT_DOWNLOAD_WRITE_BUFFER && !flushBuffer()) {
          fileError = true;
          return false;
        }
      }
      downloaded += len;
      delay(0);  // yield to other tasks / feed the watchdog
      return true;
    }
#endif
    const size_t wrote = file.write(data, len);
    downloaded += wrote;
    if (wrote != len) {
      fileError = true;
      return false;
    }
    delay(0);  // yield to other tasks / feed the watchdog
    return true;
  });

#if CROSSPOINT_DOWNLOAD_WRITE_BUFFER > 0
  // Tail flush before the file closes and before any of the size checks below,
  // which compare against `downloaded` (bytes accepted, not yet all on disk).
  if (!fileError && !flushBuffer()) {
    LOG_ERR("HTTP", "Final write flush failed");
    fileError = true;
  }
#endif

  if (fileOpen) {
    file.close();
  }

#if CROSSPOINT_DOWNLOAD_WRITE_BUFFER > 0
  const unsigned long elapsedMs = millis() - startMs;
  if (elapsedMs > 0 && downloaded > 0) {
    LOG_INF("HTTP", "Transfer: %u KB in %lu ms (%lu KB/s)", (unsigned)(downloaded >> 10), elapsedMs,
            (unsigned long)((downloaded >> 10) * 1000UL / elapsedMs));
  }
#endif

  gLastHttpStatus = httpCode > 0 ? httpCode : 0;

  // What is on disk after a resumable failure. With resumePartial the bytes
  // written so far are a valid prefix of the file (appended after a 206, or a
  // fresh start), so they are kept for the next attempt; otherwise the usual
  // delete-on-failure applies.
  const auto discardUnlessResumable = [&]() {
    if (resumePartial) {
      LOG_INF("HTTP", "Keeping %u-byte partial file for resume", (unsigned)(resumeBase + downloaded));
      return;
    }
    if (fileOpen || Storage.exists(destPath.c_str())) {
      Storage.remove(destPath.c_str());
    }
  };

  const size_t contentLength = http.hasContentLength() ? http.getContentLength() : 0;
  const size_t totalOnDisk = resumeBase + downloaded;
  // How the body's end was signalled. `framed` means the transport can tell a
  // complete body from a truncated one on its own (responseComplete() above).
  const bool chunked = http.getHeader("transfer-encoding").find("chunked") != std::string::npos;
  const bool framed = chunked || http.hasContentLength();

  // Single exit: every failure below lands in `result`, and the failure report
  // at the bottom sees the same state the decision was made on.
  DownloadError result = OK;
  if (cancelled) {
    LOG_INF("HTTP", "Download cancelled after %zu bytes", downloaded);
    discardUnlessResumable();
    result = ABORTED;
  } else if (fileError) {
    // A write failure is usually a full or removed card: a partial is of no
    // use, and deleting it is the one thing that might free space.
    LOG_ERR("HTTP", "Write failed during download");
    if (fileOpen || Storage.exists(destPath.c_str())) {
      Storage.remove(destPath.c_str());
    }
    result = FILE_ERROR;
  } else if (httpCode < 0) {
    // -1 = transport (DNS / TCP / TLS / no status line). Nothing was written
    // this call, so any resumable partial is untouched. Log where THIS device
    // sits on the network — a saved-credentials auto-join onto the wrong SSID
    // is otherwise invisible.
    const auto& diag = http.lastFailure();
    LOG_ERR("HTTP", "Download failed: no response (stage=%s, %lums, ip=%s, rssi=%d)", diag.stage, diag.elapsedMs,
            WiFi.localIP().toString().c_str(), WiFi.RSSI());
    result = CONNECT_ERROR;
  } else if (httpCode == 416 && resumeBase > 0) {
    // Range not satisfiable: the partial does not match what the server has
    // now (re-uploaded book, or it was already complete). Start clean next time.
    LOG_ERR("HTTP", "Server rejected the resume range (416); discarding partial file");
    Storage.remove(destPath.c_str());
    result = HTTP_STATUS_ERROR;
  } else if (httpCode != 200 && httpCode != 206) {
    LOG_ERR("HTTP", "Download failed: HTTP %d", httpCode);
    result = HTTP_STATUS_ERROR;
  } else if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    discardUnlessResumable();
    result = NO_DATA_ERROR;
  } else if (!http.responseComplete()) {
    // A body that stopped short of its framing (chunked terminator or
    // Content-Length) is a truncation. Reject it loudly rather than handing a
    // partial file downstream (the corrupt-cover bug).
    LOG_ERR("HTTP", "Transfer truncated: got %zu bytes without a clean end-of-body", downloaded);
    discardUnlessResumable();
    result = TRUNCATED_ERROR;
  } else if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    discardUnlessResumable();
    result = TRUNCATED_ERROR;
  } else if (expectedSize > 0 && !framed && totalOnDisk < expectedSize - expectedSize / 8) {
    // Cross-check against the size the caller knows independently (e.g. BookFusion's
    // API download_size) — but ONLY for a body with no framing at all (neither
    // Content-Length nor chunked), which ends on connection close and so cannot
    // tell a drop from the end of the file. A chunked body that reached its
    // terminator is complete whatever the API claimed: BookFusion's reader
    // endpoint serves a chunked EPUB that is ~55% of the book's advertised
    // download_size, and this check used to reject that complete file three
    // times in a row. The 87.5% band absorbs small advertised-vs-served drift.
    LOG_ERR("HTTP", "Incomplete download: got %zu bytes, expected ~%zu", totalOnDisk, expectedSize);
    discardUnlessResumable();
    result = TRUNCATED_ERROR;
  }

  if (result == OK) {
    LOG_DBG("HTTP", "Downloaded %zu bytes this session, %zu on disk (Content-Length %zu)", downloaded, totalOnDisk,
            contentLength);
    // 3. A framed body that disagrees with the caller's expected size is worth a
    // note in the log (it is what the above used to fail on), not a failure.
    if (expectedSize > 0 &&
        (totalOnDisk < expectedSize - expectedSize / 8 || totalOnDisk > expectedSize + expectedSize / 8)) {
      LOG_INF("HTTP", "Size differs from expected: got %zu, caller expected ~%zu (accepted: body was %s)", totalOnDisk,
              expectedSize, chunked ? "chunked and terminated" : "Content-Length complete");
      DownloadFailureLog::line("warning: served %u bytes but caller expected ~%u (%s); accepted", (unsigned)totalOnDisk,
                               (unsigned)expectedSize, chunked ? "chunked, terminated" : "content-length matched");
    }
    return OK;
  }

  // Failure report for the SD log (DownloadFailureLog::kPath). The TLS session
  // is closed first so its record buffers are back in the heap before the
  // report's own small allocations (the response-header dump builds a vector
  // of std::string pairs) and so the "after" heap line shows the activity's
  // footprint, not the transport's. Diagnostics and headers are plain members
  // of `http` and survive end().
  http.end();
  {
    static const char* const kNames[] = {"OK",      "HTTP_ERROR",        "FILE_ERROR",    "ABORTED",
                                         "CONNECT", "HTTP_STATUS_ERROR", "NO_DATA_ERROR", "TRUNCATED_ERROR"};
    const unsigned long now = millis();
    const auto& diag = http.lastFailure();
    DownloadFailureLog::section("HTTP transfer failed");
    DownloadFailureLog::url("url", url.c_str());
    DownloadFailureLog::line("dest=%s expectedSize=%u resumePartial=%d", destPath.c_str(), (unsigned)expectedSize,
                             resumePartial ? 1 : 0);
    if (rangeRequested > 0) {
      DownloadFailureLog::line("resume: requested Range from byte %u -> %s", (unsigned)rangeRequested,
                               httpCode == 206   ? "honoured (206)"
                               : httpCode == 200 ? "IGNORED (200, restarted from 0)"
                                                 : "no body");
    } else {
      DownloadFailureLog::line("resume: no partial to resume from");
    }
    DownloadFailureLog::line("framing: chunked=%d contentLength=%d -> %s", chunked ? 1 : 0,
                             http.hasContentLength() ? 1 : 0, framed ? "framed" : "UNFRAMED (ends on close)");
    DownloadFailureLog::line("result=%s(%d) httpCode=%d responseComplete=%d contentLength=%u(hasHeader=%d)",
                             kNames[result < 8 ? result : 1], (int)result, httpCode, http.responseComplete() ? 1 : 0,
                             (unsigned)contentLength, http.hasContentLength() ? 1 : 0);
    DownloadFailureLog::line("bytes: thisSession=%u onDisk=%u chunks=%lu fileOpen=%d fileError=%d cancelled=%d",
                             (unsigned)downloaded, (unsigned)totalOnDisk, (unsigned long)chunkCount, fileOpen ? 1 : 0,
                             fileError ? 1 : 0, cancelled ? 1 : 0);
    DownloadFailureLog::line("timing: request->now=%lums firstChunk=+%lums lastChunk=+%lums maxGap=%lums rate=%luKB/s",
                             now - requestMs, firstChunkMs ? firstChunkMs - requestMs : 0,
                             lastChunkMs ? lastChunkMs - requestMs : 0, maxChunkGapMs,
                             (lastChunkMs > firstChunkMs && downloaded > 0)
                                 ? (unsigned long)((downloaded >> 10) * 1000UL / (lastChunkMs - firstChunkMs))
                                 : 0UL);
    if (httpCode < 0) {
      DownloadFailureLog::line("transport: stage=%s elapsed=%lums partialBytes=%u available=%d connected=%d reused=%d",
                               diag.stage, diag.elapsedMs, (unsigned)diag.partialBytes, diag.available,
                               diag.connected ? 1 : 0, diag.reusedConnection ? 1 : 0);
      if (diag.partialBytes > 0) {
        DownloadFailureLog::line("transport: partial=\"%s\"", diag.partial);
      }
    } else {
      for (const auto& h : http.getHeaders()) {
        DownloadFailureLog::line("hdr: %s: %s", h.first.c_str(), h.second.c_str());
      }
    }
    {
      size_t onDiskNow = 0;
      const bool exists = Storage.exists(destPath.c_str());
      if (exists) {
        FsFile f = Storage.open(destPath.c_str(), O_RDONLY);
        if (f) {
          onDiskNow = f.fileSize();
          f.close();
        }
      }
      DownloadFailureLog::line("sd: dest exists=%d size=%u (kept for resume=%d)", exists ? 1 : 0, (unsigned)onDiskNow,
                               (exists && resumePartial) ? 1 : 0);
    }
    DownloadFailureLog::line("before: heap free=%u largest=%u", (unsigned)heapFreeBefore, (unsigned)heapLargestBefore);
    DownloadFailureLog::environment("after");
  }
  return result;
}
