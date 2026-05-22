#pragma once
#include <cstddef>
#include <cstdint>

/**
 * BookFusion reading position (EPUB).
 *
 * percentage: 0–100 (note: BookFusion uses 0-100, unlike KOReader's 0.0-1.0)
 * chapterIndex: spine index, 0-based
 * pagePositionInBook: (chapterIndex + fractional_position_in_chapter) / total_spine_count
 */
struct BookFusionPosition {
  float percentage = 0.0f;          // 0–100
  float pagePositionInBook = 0.0f;  // fractional book position
  int chapterIndex = 0;             // spine index, 0-based
  char updatedAt[40] = {};          // Server timestamp from updated_at, if present
};

/**
 * Response from the device-code auth endpoint.
 */
struct BookFusionDeviceCodeResponse {
  char deviceCode[256] = {};
  char userCode[16] = {};
  char verificationUri[128] = {};
  int interval = 5;     // seconds between polls
  int expiresIn = 600;  // seconds until code expires
};

/**
 * A single book from the user's BookFusion library.
 */
struct BookFusionBook {
  uint32_t id = 0;
  char title[64] = {};
  char authors[48] = {};
  char format[8] = {};  // "EPUB", "PDF", etc.
  char coverUrl[384] = {};
};

/**
 * Paginated result from the book search endpoint.
 * Stores one display page. The search request asks BookFusion for one extra
 * item, but that sentinel is only used to set hasMore and is not retained.
 */
struct BookFusionSearchResult {
  static constexpr int MAX_BOOKS = 10;
  BookFusionBook books[MAX_BOOKS];
  int count = 0;        // Books in this page.
  int totalCount = 0;   // Total books across all pages (from the Total-Count
                        // response header). 0 means the server didn't tell us.
  int currentPage = 0;
  bool hasMore = false;
};

/**
 * A single user-created bookshelf from BookFusion.
 * Plugin source (bf_browser.lua) only reads `id` and `name`, so that's all
 * we parse — keeps the per-shelf footprint to 52 bytes.
 */
struct BookFusionBookshelf {
  uint32_t id = 0;
  char name[48] = {};
};

/**
 * Full list of the user's bookshelves. The /api/user/bookshelves/search
 * endpoint accepts no pagination params in the KOReader plugin's usage, so
 * we assume it returns all shelves in one response. We cap at MAX_SHELVES
 * (~1.6 KB) to bound heap usage; extra shelves are silently dropped.
 */
struct BookFusionBookshelfList {
  // Server-side pagination has been added to the bookshelves endpoint (we ask
  // for 32 per request and walk pages until we've got everything or hit the
  // cap), so the cap can be larger than one server-page. 64 covers virtually
  // every user; ~3.3 KB on the activity heap is well within budget.
  static constexpr int MAX_SHELVES = 64;
  BookFusionBookshelf shelves[MAX_SHELVES];
  int count = 0;
};

/**
 * HTTP client for the BookFusion API.
 *
 * Base URL: https://www.bookfusion.com
 * All authenticated requests use: Authorization: Bearer <token>
 *                                 Accept: application/json; api_version=10
 *
 * Authentication uses the OAuth 2.0 Device Code flow:
 *   1. requestDeviceCode() → display verificationUri + userCode to user
 *   2. pollForToken() every interval seconds → returns OK + token when authorised
 *
 * Progress API:
 *   getProgress(bookId, out)  → GET /api/user/books/{id}/reading_position
 *   setProgress(bookId, pos)  → POST /api/user/books/{id}/reading_position
 */
class BookFusionSyncClient {
 public:
  enum Error {
    OK = 0,
    NO_TOKEN,       // BF_TOKEN_STORE has no token
    NETWORK_ERROR,  // HTTP/TLS failure
    AUTH_FAILED,    // 401 Unauthorized
    SERVER_ERROR,   // 5xx or unexpected code
    JSON_ERROR,     // Failed to parse response
    NOT_FOUND,      // 404 — no progress exists yet
    PENDING,        // authorization_pending — keep polling
    SLOW_DOWN,      // slow_down — increase poll interval
    EXPIRED,        // expired_token
    DENIED,         // access_denied by user
  };

  // --- Device Code Auth (unauthenticated) ---
  static Error requestDeviceCode(BookFusionDeviceCodeResponse& out);
  static Error pollForToken(const char* deviceCode, char* outToken, size_t tokenMaxLen);

  // --- Progress ---
  static Error getProgress(uint32_t bookId, BookFusionPosition& out);
  static Error setProgress(uint32_t bookId, const BookFusionPosition& pos, BookFusionPosition* out = nullptr);

  // --- Library Browse & Download ---
  // bookshelfId, when non-zero, restricts the search to a specific user bookshelf
  // (sent as `bookshelf_id` in the request body alongside list/sort/page).
  static Error searchBooks(int page, BookFusionSearchResult& out, const char* list = nullptr,
                           const char* sort = nullptr, uint32_t bookshelfId = 0);
  static Error getDownloadUrl(uint32_t bookId, char* outUrl, size_t maxLen);

  // --- Bookshelves ---
  // Returns the user's custom shelves. POSTs `{}` to /api/user/bookshelves/search.
  // The plugin source treats this as un-paginated; we accept that.
  static Error searchBookshelves(BookFusionBookshelfList& out);

  static const char* errorString(Error error);

  static constexpr char API_ACCEPT[] = "application/json; api_version=10";

 private:
  static constexpr char BASE_URL[] = "https://www.bookfusion.com";
  static constexpr char CLIENT_ID[] = "koreader";
  static constexpr char DEVICE_CODE_GRANT_TYPE[] = "urn:ietf:params:oauth:grant-type:device_code";
};
