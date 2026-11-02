#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
  // Flat JSON object of service ids sent verbatim inside metadata (JSON types
  // kept): the book's plugin sidecar ("<book>.meta.json") plus our own
  // BookFusion download id as "bookfusion_id". crosspoint-sync routes progress
  // to a linked service with it; the firmware assigns no meaning to the keys.
  std::string extraJson;
  std::string isbn;    // ISBN-13 from the package metadata, when the book has one
  std::string asin;    // Amazon ASIN from the package metadata, when the book has one
  std::string series;  // Series title, when the book has one
  std::optional<float> seriesIndex;

  // Snapshot the metadata of an open, loaded book, or nothing when the user has
  // Send Book Details off. Reads only what the book already has cached (the
  // loaded book.bin fields and the 13-byte ISBN sidecar), so it never re-parses
  // the OPF. Call it while the Epub is alive; the result owns its strings.
  static std::optional<KOReaderMetadata> fromBook(const Epub& epub);
};

/**
 * Rich CrossPoint position sent alongside progress uploads. Maps 1:1 onto the
 * crosspoint-sync extended `position` object (crosspoint-sync docs/API.md).
 * Sent only when the server type supports rich progress. Page fields are
 * layout hints; the standard XPath stays the content anchor.
 */
struct KOReaderRichPosition {
  uint32_t pctQ = 0;                       // Percentage quantized 0..1,000,000
  uint16_t spineIndex = 0;                 // Spine (chapter) index
  uint16_t pageNumber = 0;                 // Page within spine (layout-dependent hint)
  uint16_t totalPages = 1;                 // Spine page count (layout-dependent hint)
  std::optional<uint16_t> paragraphIndex;  // Synthetic 1-based paragraph index
  std::string xpath;                       // KOReader-style xpath (server cap: 120 bytes)
};

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;                          // Document hash
  std::string progress;                          // XPath-like progress string
  float percentage = 0.0f;                       // Progress percentage (0.0 to 1.0)
  std::string device;                            // Device name
  std::string deviceId;                          // Device ID
  int64_t timestamp = 0;                         // Unix timestamp of last update
  std::optional<KOReaderMetadata> metadata;      // Upload only; never filled by getProgress
  std::optional<KOReaderRichPosition> position;  // Rich position, both directions, when supported
  // Upload only: `progress` is a guessed XPath (KOReaderPosition::structural
  // was false). Refused for a strict KOSync server; a crosspoint-sync server
  // still takes it, since its receivers fall back to the rich position.
  bool progressIsGuess = false;
};

/**
 * One entry of the CrossPoint Sync progress change feed
 * (GET /api/v1/progress/changes): a document's newest position, with every
 * hash the server knows it by (a merged document has aliases).
 */
struct KOReaderProgressChange {
  // A merged document rarely has more than a couple of aliases.
  static constexpr uint8_t MAX_ALIASES = 4;
  std::string document;  // canonical hash
  std::string aliases[MAX_ALIASES];
  uint8_t aliasCount = 0;
  float percentage = 0.0f;
  std::string progress;  // KOReader XPath
  std::optional<KOReaderRichPosition> position;
  std::string device;
  int64_t timestamp = 0;

  // True when `hash` is the canonical id or one of the aliases.
  bool matches(const std::string& hash) const;
};

/**
 * Body of a PUT /api/v1/progress/batch, built one item at a time so only the
 * serialized JSON is alive during the TLS request, never a list of
 * KOReaderProgress records (and the Epub each was mapped from).
 */
class KOReaderProgressBatch {
 public:
  // False when the batch is full (MAX_PROGRESS_BATCH) or the item is invalid.
  bool add(const KOReaderProgress& progress);
  size_t count() const { return count_; }
  void clear();

 private:
  friend class KOReaderSyncClient;
  std::string body_;
  size_t count_ = 0;
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
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    LOW_MEMORY,
    USER_EXISTS,
    POSITION_UNRESOLVED,  // upload refused: the local position has no structural XPath
    FEED_UNSUPPORTED,     // strict KOSync or an older CrossPoint Sync: no change feed
    BATCH_UNSUPPORTED     // no PUT /api/v1/progress/batch: send one PUT per book
  };

  // Positions per batch request. The server takes 20; 8 keeps the body (held
  // in RAM for the whole TLS request) to ~4KB on the C3.
  static constexpr size_t MAX_PROGRESS_BATCH = 8;
  // Changes per feed page. The server stops a page before 8KB anyway; 10
  // bounds the parsed list (~10 x 250 bytes) as well.
  static constexpr uint8_t MAX_CHANGES_PAGE = 10;
  // Server cap on one change-feed page body.
  static constexpr uint32_t MAX_CHANGES_BODY = 8 * 1024;

  struct ChangesPage {
    uint32_t cursor = 0;  // where the next page starts
    bool more = false;
    uint16_t count = 0;
  };

  /**
   * Authenticate with the sync server (validate credentials).
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Register the stored username/password as a new account (POST /users/create).
   * @return OK on success, USER_EXISTS if the username is taken, error code on failure
   */
  static Error createUser();

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
   * PUT /api/v1/progress/batch (CrossPoint/Other servers only): every item in
   * one request, all stored or none. Empties `batch` whatever the outcome.
   * @return OK, BATCH_UNSUPPORTED (no route, or a strict KOSync server), or an error
   */
  static Error updateProgressBatch(KOReaderProgressBatch& batch);

  /**
   * GET /api/v1/progress/changes?since=&limit=&device= (CrossPoint/Other
   * servers only): documents whose newest position changed after `since`,
   * leaving out the ones this reader wrote last. `out` is replaced.
   * @return OK, FEED_UNSUPPORTED (no route, or a strict KOSync server), or an error
   */
  static Error getProgressChanges(uint32_t since, uint8_t limit, ChangesPage& page,
                                  std::vector<KOReaderProgressChange>& out);

  // One progress record as JSON, shared by the single and batch uploads.
  // Applies the server-type rules (metadata switch, extended fields, rich
  // position, per-reader device id).
  static void fillProgressJson(JsonObject doc, const KOReaderProgress& progress);

  /**
   * Get human-readable error message.
   */
  static const char* errorString(Error error);

  // Characters readerId() writes, including the terminator.
  static constexpr size_t READER_ID_SIZE = 28;

  /**
   * This reader's own sync device id, "katiepoint-" + 16 hex digits: a salted
   * MD5 of the factory MAC, so it is stable across SD cards and reflashes but
   * never carries the MAC itself. Sent as `device_id` to CrossPoint/Other
   * servers, where it gives each reader its own progress row (so the change
   * feed can leave out this reader's writes) and scopes its stats snapshots.
   * A strict KOSync server keeps the shared legacy id. False only when the
   * buffer is too small.
   */
  static bool readerId(char* out, size_t capacity);

  /** device_id for a request to the configured server type (see readerId). */
  static void deviceIdForServer(char* out, size_t capacity);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;
};
