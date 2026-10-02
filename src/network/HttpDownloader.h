#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps freeink::SecureHttpClient (wolfSSL TLS 1.3) for HTTPS requests.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // One outgoing request header, as (name, value). SD plugins express their
  // auth as manifest-declared headers, so the caller supplies them per request
  // rather than the downloader knowing any scheme.
  using Header = std::pair<std::string, std::string>;

  // Why a download failed. The first four are the historical set; the rest
  // split HTTP_ERROR into causes a user can act on (see lastHttpStatus()).
  enum DownloadError {
    OK = 0,
    HTTP_ERROR,         // Unclassified (malformed URL)
    FILE_ERROR,         // SD card: could not create or write the destination
    ABORTED,            // The caller's cancelFlag fired
    CONNECT_ERROR,      // No HTTP status line: DNS / TCP / TLS / server silent
    HTTP_STATUS_ERROR,  // Server answered with a non-2xx status (lastHttpStatus())
    NO_DATA_ERROR,      // 200 with an empty body
    TRUNCATED_ERROR,    // Body stopped short of its framing or the expected size
  };

  // HTTP status of the most recent downloadToFile() call (0 if none arrived).
  // Single-task use only: downloads never overlap on this firmware.
  static int lastHttpStatus();

  /**
   * Fetch text content from a URL.
   * @param url The URL to fetch
   * @param outContent The fetched content (output)
   * @return true if fetch succeeded, false on error
   */
  static bool fetchUrl(const std::string& url, std::string& outContent);

  static bool fetchUrl(const std::string& url, Stream& stream);

  /**
   * Download a file to the SD card.
   * @param url The URL to download
   * @param destPath The destination path on SD card
   * @param progress Optional progress callback
   * @param allowConfiguredAuth Whether to attach configured OPDS Basic auth for non-BookFusion URLs
   * @param expectedSize Optional expected file size in bytes (e.g. BookFusion's API
   *        download_size). When non-zero, a final file that falls well short of this
   *        is rejected as a truncated download. 0 = no cross-check.
   * @param cancelFlag Optional abort switch, polled once per received chunk. When it
   *        turns true the transfer stops, the partial file is deleted and ABORTED is
   *        returned. The caller owns the flag; it is only read here. Written from the
   *        progress callback on the same task (nothing here runs on another task), so
   *        `volatile` is documentation of intent rather than a memory barrier.
   * @param username Optional HTTP Basic user for this request only (SD plugins carry
   *        their own credentials; the configured OPDS ones must not leak to them).
   * @param password Password for `username`.
   * @param headers Extra request headers, e.g. a plugin's Authorization line.
   * @param resumePartial Resume an interrupted transfer. When destPath already
   *        exists its bytes are kept and the request carries `Range: bytes=N-`;
   *        a 206 reply is appended to the file, a 200 reply (server ignored the
   *        range) restarts it from scratch. On a resumable failure (connection
   *        dropped, truncated body, cancelled) the partial file is KEPT so the
   *        next call can continue; the caller must therefore download to a
   *        scratch name and rename on OK, or a half file is left where a
   *        reader could open it. Without this flag any failure deletes the file.
   * @return DownloadError indicating success or failure type
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool allowConfiguredAuth = true,
                                      size_t expectedSize = 0, const volatile bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      const std::vector<Header>& headers = {}, bool resumePartial = false);
};
