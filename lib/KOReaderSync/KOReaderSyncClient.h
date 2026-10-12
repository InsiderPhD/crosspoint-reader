#pragma once
#include <cstdint>
#include <optional>
#include <string>

class Epub;

/**
 * Optional document metadata sent alongside a progress upload. Mirrors the
 * metadata object added in KOReader PR #15306 (filename/title/authors); the
 * standard sync server ignores it, custom servers may use it. The remaining
 * fields are only serialized when the server type is OTHER.
 */
struct KOReaderMetadata {
  std::string filename;  // e.g. "my_book.epub"
  std::string title;
  std::string authors;
  std::string isbn;    // ISBN-13 from the package metadata, when the book has one
  std::string series;  // Series title, when the book has one
  std::optional<float> seriesIndex;

  // Snapshot the metadata of an open, loaded book, or nothing when the user has
  // Send Book Details off. Reads only what the book already has cached (the
  // loaded book.bin fields and the 13-byte ISBN sidecar), so it never re-parses
  // the OPF. Call it while the Epub is alive; the result owns its strings.
  static std::optional<KOReaderMetadata> fromBook(const Epub& epub);
};

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;                      // Document hash
  std::string progress;                      // XPath-like progress string
  float percentage = 0.0f;                   // Progress percentage (0.0 to 1.0)
  std::string device;                        // Device name
  std::string deviceId;                      // Device ID
  int64_t timestamp = 0;                     // Unix timestamp of last update
  std::optional<KOReaderMetadata> metadata;  // Upload only; never filled by getProgress
};

/**
 * HTTP client for KOReader sync API.
 *
 * Base URL: https://sync.koreader.rocks:443/
 *
 * API Endpoints:
 *   GET /users/auth - Authenticate (validate credentials)
 *   GET /syncs/progress/:document - Get progress for a document
 *   PUT /syncs/progress - Update progress for a document
 *
 * Authentication:
 *   x-auth-user: username
 *   x-auth-key: MD5 hash of password
 */
class KOReaderSyncClient {
 public:
  enum Error { OK = 0, NO_CREDENTIALS, NETWORK_ERROR, AUTH_FAILED, SERVER_ERROR, JSON_ERROR, NOT_FOUND };

  /**
   * Authenticate with the sync server (validate credentials).
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Get reading progress for a document.
   * @param documentHash The document hash (from KOReaderDocumentId)
   * @param outProgress Output: the progress data
   * @return OK on success, NOT_FOUND if no progress exists, error code on failure
   */
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress);

  /**
   * Update reading progress for a document.
   * @param progress The progress data to upload
   * @return OK on success, error code on failure
   */
  static Error updateProgress(const KOReaderProgress& progress);

  /**
   * Get human-readable error message.
   */
  static const char* errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;
};
