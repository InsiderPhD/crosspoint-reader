#include "BookFusionSyncClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <Stream.h>
#include <WiFiClientSecure.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BookFusionTokenStore.h"
#include "StreamingJsonParser.h"

namespace {
// Add auth and accept headers to an authenticated request.
void addAuthHeaders(HTTPClient& http) {
  const std::string bearer = "Bearer " + BF_TOKEN_STORE.getToken();
  http.addHeader("Authorization", bearer.c_str());
  http.addHeader("Accept", BookFusionSyncClient::API_ACCEPT);
}

bool keyEquals(const char* key, size_t len, const char* expected) {
  return strlen(expected) == len && memcmp(key, expected, len) == 0;
}

void safeCopyToken(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  if (dstSize == 0) return;
  const size_t n = (srcLen < dstSize - 1) ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

void safeAppendToken(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  if (dstSize == 0 || srcLen == 0) return;
  const size_t used = strlen(dst);
  if (used >= dstSize - 1) return;
  const size_t available = dstSize - 1 - used;
  const size_t n = (srcLen < available) ? srcLen : available;
  memcpy(dst + used, src, n);
  dst[used + n] = '\0';
}

void appendAuthor(BookFusionBook& book, const char* name, size_t len) {
  if (len == 0) return;
  if (book.authors[0] != '\0') {
    safeAppendToken(book.authors, sizeof(book.authors), ", ", 2);
  }
  safeAppendToken(book.authors, sizeof(book.authors), name, len);
}

void readBookFusionPosition(JsonDocument& doc, BookFusionPosition& out) {
  out.percentage = doc["percentage"] | 0.0f;
  out.chapterIndex = doc["chapter_index"] | 0;
  out.pagePositionInBook = doc["page_position_in_book"] | 0.0f;
  strlcpy(out.updatedAt, doc["updated_at"] | "", sizeof(out.updatedAt));
}

class BookFusionSearchJsonStream final : public Stream {
 public:
  BookFusionSearchJsonStream(BookFusionSearchResult& out, int page)
      : out_(out),
        parser_(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                              sOnArrayStart, sOnArrayEnd}) {
    out_.count = 0;
    out_.currentPage = page;
    out_.hasMore = false;
  }

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    bytesRead_ += size;
    parser_.feed(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t bytesRead() const { return bytesRead_; }
  bool empty() const { return bytesRead_ == 0; }
  bool ok() const { return bytesRead_ > 0 && rootArraySeen_ && rootArrayClosed_ && depth_ == 0 && !parser_.hasError(); }

 private:
  enum class LastKey : uint8_t {
    NONE,
    ID,
    TITLE,
    FORMAT,
    COVER,
    COVER_URL,
    AUTHORS,
    AUTHOR_NAME,
  };

  static void sOnKey(void* ctx, const char* key, size_t len) {
    static_cast<BookFusionSearchJsonStream*>(ctx)->onKey(key, len);
  }

  static void sOnString(void* ctx, const char* value, size_t len) {
    static_cast<BookFusionSearchJsonStream*>(ctx)->onString(value, len);
  }

  static void sOnNumber(void* ctx, const char* value, size_t len) {
    static_cast<BookFusionSearchJsonStream*>(ctx)->onNumber(value, len);
  }

  static void sOnBool(void* ctx, bool /*value*/) {
    static_cast<BookFusionSearchJsonStream*>(ctx)->lastKey_ = LastKey::NONE;
  }

  static void sOnNull(void* ctx) { static_cast<BookFusionSearchJsonStream*>(ctx)->lastKey_ = LastKey::NONE; }

  static void sOnObjectStart(void* ctx) { static_cast<BookFusionSearchJsonStream*>(ctx)->onObjectStart(); }

  static void sOnObjectEnd(void* ctx) { static_cast<BookFusionSearchJsonStream*>(ctx)->onObjectEnd(); }

  static void sOnArrayStart(void* ctx) { static_cast<BookFusionSearchJsonStream*>(ctx)->onArrayStart(); }

  static void sOnArrayEnd(void* ctx) { static_cast<BookFusionSearchJsonStream*>(ctx)->onArrayEnd(); }

  void onKey(const char* key, size_t len) {
    lastKey_ = LastKey::NONE;

    if (inBook_ && depth_ == bookDepth_) {
      if (keyEquals(key, len, "id")) {
        lastKey_ = LastKey::ID;
      } else if (keyEquals(key, len, "title")) {
        lastKey_ = LastKey::TITLE;
      } else if (keyEquals(key, len, "format")) {
        lastKey_ = LastKey::FORMAT;
      } else if (keyEquals(key, len, "cover")) {
        lastKey_ = LastKey::COVER;
      } else if (keyEquals(key, len, "authors")) {
        lastKey_ = LastKey::AUTHORS;
      }
    } else if (inCover_ && depth_ == coverDepth_ && keyEquals(key, len, "url")) {
      lastKey_ = LastKey::COVER_URL;
    } else if (inAuthor_ && depth_ == authorDepth_ && keyEquals(key, len, "name")) {
      lastKey_ = LastKey::AUTHOR_NAME;
    }
  }

  void onString(const char* value, size_t len) {
    switch (lastKey_) {
      case LastKey::ID:
        currentBook_.id = static_cast<uint32_t>(strtoul(value, nullptr, 10));
        break;
      case LastKey::TITLE:
        safeCopyToken(currentBook_.title, sizeof(currentBook_.title), value, len);
        break;
      case LastKey::FORMAT:
        safeCopyToken(currentBook_.format, sizeof(currentBook_.format), value, len);
        break;
      case LastKey::COVER_URL:
        safeCopyToken(currentBook_.coverUrl, sizeof(currentBook_.coverUrl), value, len);
        break;
      case LastKey::AUTHOR_NAME:
        appendAuthor(currentBook_, value, len);
        break;
      default:
        break;
    }
    lastKey_ = LastKey::NONE;
  }

  void onNumber(const char* value, size_t /*len*/) {
    if (lastKey_ == LastKey::ID) {
      currentBook_.id = static_cast<uint32_t>(strtoul(value, nullptr, 10));
    }
    lastKey_ = LastKey::NONE;
  }

  void onObjectStart() {
    if (rootArraySeen_ && !inBook_ && depth_ == 1) {
      beginBook(depth_ + 1);
    } else if (inBook_ && lastKey_ == LastKey::COVER && depth_ == bookDepth_) {
      inCover_ = true;
      coverDepth_ = depth_ + 1;
    } else if (inAuthors_ && depth_ == authorsDepth_) {
      inAuthor_ = true;
      authorDepth_ = depth_ + 1;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onObjectEnd() {
    if (inAuthor_ && depth_ == authorDepth_) {
      inAuthor_ = false;
      authorDepth_ = 0;
    } else if (inCover_ && depth_ == coverDepth_) {
      inCover_ = false;
      coverDepth_ = 0;
    } else if (inBook_ && depth_ == bookDepth_) {
      commitBook();
      inBook_ = false;
      bookDepth_ = 0;
    }
    if (depth_ > 0) --depth_;
    lastKey_ = LastKey::NONE;
  }

  void onArrayStart() {
    if (depth_ == 0) {
      rootArraySeen_ = true;
    } else if (inBook_ && lastKey_ == LastKey::AUTHORS && depth_ == bookDepth_) {
      inAuthors_ = true;
      authorsDepth_ = depth_ + 1;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onArrayEnd() {
    if (inAuthors_ && depth_ == authorsDepth_) {
      inAuthors_ = false;
      authorsDepth_ = 0;
    }
    if (rootArraySeen_ && depth_ == 1) {
      rootArrayClosed_ = true;
    }
    if (depth_ > 0) --depth_;
    lastKey_ = LastKey::NONE;
  }

  void beginBook(uint8_t bookDepth) {
    currentBook_ = BookFusionBook{};
    safeCopyToken(currentBook_.title, sizeof(currentBook_.title), "Untitled", 8);
    safeCopyToken(currentBook_.format, sizeof(currentBook_.format), "epub", 4);
    bookDepth_ = bookDepth;
    inBook_ = true;
  }

  void commitBook() {
    if (currentBook_.id == 0) return;
    if (out_.count >= BookFusionSearchResult::MAX_BOOKS) {
      out_.hasMore = true;
      return;
    }
    out_.books[out_.count++] = currentBook_;
  }

  BookFusionSearchResult& out_;
  StreamingJsonParser parser_;
  BookFusionBook currentBook_;
  LastKey lastKey_ = LastKey::NONE;
  size_t bytesRead_ = 0;
  uint8_t depth_ = 0;
  uint8_t bookDepth_ = 0;
  uint8_t coverDepth_ = 0;
  uint8_t authorsDepth_ = 0;
  uint8_t authorDepth_ = 0;
  bool rootArraySeen_ = false;
  bool rootArrayClosed_ = false;
  bool inBook_ = false;
  bool inCover_ = false;
  bool inAuthors_ = false;
  bool inAuthor_ = false;
};

class BookFusionBookshelfJsonStream final : public Stream {
 public:
  // Doesn't reset out.count — callers manage it so we can accumulate across
  // pages when searchBookshelves iterates the paginated endpoint.
  explicit BookFusionBookshelfJsonStream(BookFusionBookshelfList& out)
      : out_(out),
        parser_(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                              sOnArrayStart, sOnArrayEnd}) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    bytesRead_ += size;
    parser_.feed(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t bytesRead() const { return bytesRead_; }
  bool capped() const { return capped_; }
  bool empty() const { return bytesRead_ == 0; }
  bool ok() const { return bytesRead_ > 0 && rootArraySeen_ && rootArrayClosed_ && depth_ == 0 && !parser_.hasError(); }

 private:
  enum class LastKey : uint8_t {
    NONE,
    ID,
    NAME,
  };

  static void sOnKey(void* ctx, const char* key, size_t len) {
    static_cast<BookFusionBookshelfJsonStream*>(ctx)->onKey(key, len);
  }

  static void sOnString(void* ctx, const char* value, size_t len) {
    static_cast<BookFusionBookshelfJsonStream*>(ctx)->onString(value, len);
  }

  static void sOnNumber(void* ctx, const char* value, size_t len) {
    static_cast<BookFusionBookshelfJsonStream*>(ctx)->onNumber(value, len);
  }

  static void sOnBool(void* ctx, bool /*value*/) {
    static_cast<BookFusionBookshelfJsonStream*>(ctx)->lastKey_ = LastKey::NONE;
  }

  static void sOnNull(void* ctx) { static_cast<BookFusionBookshelfJsonStream*>(ctx)->lastKey_ = LastKey::NONE; }

  static void sOnObjectStart(void* ctx) { static_cast<BookFusionBookshelfJsonStream*>(ctx)->onObjectStart(); }

  static void sOnObjectEnd(void* ctx) { static_cast<BookFusionBookshelfJsonStream*>(ctx)->onObjectEnd(); }

  static void sOnArrayStart(void* ctx) { static_cast<BookFusionBookshelfJsonStream*>(ctx)->onArrayStart(); }

  static void sOnArrayEnd(void* ctx) { static_cast<BookFusionBookshelfJsonStream*>(ctx)->onArrayEnd(); }

  void onKey(const char* key, size_t len) {
    lastKey_ = LastKey::NONE;

    if (inShelf_ && depth_ == shelfDepth_) {
      if (keyEquals(key, len, "id")) {
        lastKey_ = LastKey::ID;
      } else if (keyEquals(key, len, "name")) {
        lastKey_ = LastKey::NAME;
      }
    }
  }

  void onString(const char* value, size_t len) {
    if (lastKey_ == LastKey::ID) {
      currentShelf_.id = static_cast<uint32_t>(strtoul(value, nullptr, 10));
    } else if (lastKey_ == LastKey::NAME) {
      safeCopyToken(currentShelf_.name, sizeof(currentShelf_.name), value, len);
    }
    lastKey_ = LastKey::NONE;
  }

  void onNumber(const char* value, size_t /*len*/) {
    if (lastKey_ == LastKey::ID) {
      currentShelf_.id = static_cast<uint32_t>(strtoul(value, nullptr, 10));
    }
    lastKey_ = LastKey::NONE;
  }

  void onObjectStart() {
    if (rootArraySeen_ && !inShelf_ && depth_ == 1) {
      currentShelf_ = BookFusionBookshelf{};
      safeCopyToken(currentShelf_.name, sizeof(currentShelf_.name), "Unnamed", 7);
      shelfDepth_ = depth_ + 1;
      inShelf_ = true;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onObjectEnd() {
    if (inShelf_ && depth_ == shelfDepth_) {
      commitShelf();
      inShelf_ = false;
      shelfDepth_ = 0;
    }
    if (depth_ > 0) --depth_;
    lastKey_ = LastKey::NONE;
  }

  void onArrayStart() {
    if (depth_ == 0) {
      rootArraySeen_ = true;
    }
    ++depth_;
    lastKey_ = LastKey::NONE;
  }

  void onArrayEnd() {
    if (rootArraySeen_ && depth_ == 1) {
      rootArrayClosed_ = true;
    }
    if (depth_ > 0) --depth_;
    lastKey_ = LastKey::NONE;
  }

  void commitShelf() {
    if (currentShelf_.id == 0) return;
    if (out_.count >= BookFusionBookshelfList::MAX_SHELVES) {
      capped_ = true;
      return;
    }
    out_.shelves[out_.count++] = currentShelf_;
  }

  BookFusionBookshelfList& out_;
  StreamingJsonParser parser_;
  BookFusionBookshelf currentShelf_;
  LastKey lastKey_ = LastKey::NONE;
  size_t bytesRead_ = 0;
  uint8_t depth_ = 0;
  uint8_t shelfDepth_ = 0;
  bool rootArraySeen_ = false;
  bool rootArrayClosed_ = false;
  bool inShelf_ = false;
  bool capped_ = false;
};
}  // namespace

// --- Device Code Auth ---

BookFusionSyncClient::Error BookFusionSyncClient::requestDeviceCode(BookFusionDeviceCodeResponse& out) {
  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/auth/device", BASE_URL);
  LOG_DBG("BFS", "Requesting device code: %s", url);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["client_id"] = CLIENT_ID;
  String bodyStr;
  serializeJson(body, bodyStr);

  const int httpCode = http.POST(bodyStr);
  String responseBody = http.getString();
  http.end();

  LOG_DBG("BFS", "requestDeviceCode response: %d", httpCode);

  if (httpCode < 0) {
    return NETWORK_ERROR;
  }
  if (httpCode != 200) {
    return SERVER_ERROR;
  }

  JsonDocument doc;
  if (deserializeJson(doc, responseBody) != DeserializationError::Ok) {
    LOG_ERR("BFS", "requestDeviceCode JSON parse error");
    return JSON_ERROR;
  }

  strlcpy(out.deviceCode, doc["device_code"] | "", sizeof(out.deviceCode));
  strlcpy(out.userCode, doc["user_code"] | "", sizeof(out.userCode));
  strlcpy(out.verificationUri, doc["verification_uri"] | "", sizeof(out.verificationUri));
  out.interval = doc["interval"] | 5;
  out.expiresIn = doc["expires_in"] | 600;

  LOG_DBG("BFS", "Device code received: user_code=%s, interval=%ds, expires_in=%ds", out.userCode, out.interval,
          out.expiresIn);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::pollForToken(const char* deviceCode, char* outToken,
                                                               size_t tokenMaxLen) {
  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/auth/token", BASE_URL);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["grant_type"] = DEVICE_CODE_GRANT_TYPE;
  body["client_id"] = CLIENT_ID;
  body["device_code"] = deviceCode;
  String bodyStr;
  serializeJson(body, bodyStr);

  const int httpCode = http.POST(bodyStr);
  String responseBody = http.getString();
  http.end();

  LOG_DBG("BFS", "pollForToken response: %d", httpCode);

  if (httpCode < 0) {
    return NETWORK_ERROR;
  }

  JsonDocument doc;
  if (deserializeJson(doc, responseBody) != DeserializationError::Ok) {
    LOG_ERR("BFS", "pollForToken JSON parse error");
    return JSON_ERROR;
  }

  if (httpCode == 200) {
    const char* token = doc["access_token"] | "";
    if (token[0] == '\0') {
      return JSON_ERROR;
    }
    strlcpy(outToken, token, tokenMaxLen);
    LOG_DBG("BFS", "Token received");
    return OK;
  }

  // Map OAuth error codes
  const char* errCode = doc["error"] | "";
  LOG_DBG("BFS", "pollForToken error: %s", errCode);

  if (strcmp(errCode, "authorization_pending") == 0) return PENDING;
  if (strcmp(errCode, "slow_down") == 0) return SLOW_DOWN;
  if (strcmp(errCode, "expired_token") == 0) return EXPIRED;
  if (strcmp(errCode, "access_denied") == 0) return DENIED;
  // BookFusion returns "invalid_grant" (HTTP 400) while authorization is still
  // pending — non-standard, but the official Lua plugin keeps polling on any
  // unrecognised error, so we do the same.
  if (strcmp(errCode, "invalid_grant") == 0) return PENDING;

  return SERVER_ERROR;
}

// --- Progress ---

BookFusionSyncClient::Error BookFusionSyncClient::getProgress(uint32_t bookId, BookFusionPosition& out) {
  if (!BF_TOKEN_STORE.hasToken()) {
    return NO_TOKEN;
  }

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)bookId);
  LOG_DBG("BFS", "getProgress: %s", url);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  addAuthHeaders(http);

  const int httpCode = http.GET();

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, responseBody) != DeserializationError::Ok) {
      LOG_ERR("BFS", "getProgress JSON parse error");
      return JSON_ERROR;
    }

    readBookFusionPosition(doc, out);

    LOG_DBG("BFS", "Remote progress: %.2f%%, chapter %d, updated_at=%s", out.percentage, out.chapterIndex,
            out.updatedAt);
    return OK;
  }

  http.end();
  LOG_DBG("BFS", "getProgress response: %d", httpCode);

  if (httpCode == 404) return NOT_FOUND;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
}

BookFusionSyncClient::Error BookFusionSyncClient::setProgress(uint32_t bookId, const BookFusionPosition& pos,
                                                              BookFusionPosition* out) {
  if (!BF_TOKEN_STORE.hasToken()) {
    return NO_TOKEN;
  }

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)bookId);
  LOG_DBG("BFS", "setProgress: %s (%.2f%%)", url, pos.percentage);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["percentage"] = pos.percentage;
  body["chapter_index"] = pos.chapterIndex;
  body["page_position_in_book"] = pos.pagePositionInBook;
  String bodyStr;
  serializeJson(body, bodyStr);

  const int httpCode = http.POST(bodyStr);
  String responseBody;
  if (httpCode == 200 || httpCode == 201) {
    responseBody = http.getString();
  }
  http.end();

  LOG_DBG("BFS", "setProgress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 201) {
    if (out != nullptr && responseBody.length() > 0) {
      JsonDocument doc;
      if (deserializeJson(doc, responseBody) == DeserializationError::Ok) {
        readBookFusionPosition(doc, *out);
      } else {
        LOG_DBG("BFS", "setProgress response JSON parse skipped");
      }
    }
    return OK;
  }
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
}

// --- Library Browse & Download ---

BookFusionSyncClient::Error BookFusionSyncClient::searchBooks(int page, BookFusionSearchResult& out, const char* list,
                                                              const char* sort, uint32_t bookshelfId) {
  if (!BF_TOKEN_STORE.hasToken()) return NO_TOKEN;

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/search", BASE_URL);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  // BookFusion emits a `Total-Count` response header with the total number of
  // books across all pages (confirmed empirically via header dump). HTTPClient
  // requires us to register the key up-front; it's then readable via
  // http.header("Total-Count") after the request. collectHeaders takes
  // `const char**`, so the array can't be `const`-of-`const`.
  static const char* kCollectedHeaders[] = {"Total-Count"};
  http.collectHeaders(kCollectedHeaders, sizeof(kCollectedHeaders) / sizeof(kCollectedHeaders[0]));

  // Keep one visible page in memory. The response body itself is streamed
  // through BookFusionSearchJsonStream, so we never allocate the raw JSON.
  static constexpr int BOOKS_PER_PAGE = BookFusionSearchResult::MAX_BOOKS;

  JsonDocument reqBody;
  reqBody["page"] = page;
  // Ask the server for exactly BOOKS_PER_PAGE. The previous `+1` trick (request
  // one extra book to detect hasMore from overflow) meant the server then
  // paginated in groups of `per_page+1`, so every page boundary lost one book
  // and totals that landed badly on the modulo produced an empty final page.
  // Now that we have the Total-Count response header we derive hasMore from
  // totalCount instead, and pages align cleanly with the server.
  reqBody["per_page"] = BOOKS_PER_PAGE;
  reqBody["sort"] = (sort != nullptr) ? sort : "added_at-desc";
  if (list != nullptr) {
    reqBody["list"] = list;
  }
  if (bookshelfId != 0) {
    reqBody["bookshelf_id"] = bookshelfId;
  }
  String bodyStr;
  serializeJson(reqBody, bodyStr);

  const int httpCode = http.POST(bodyStr);
  LOG_DBG("BFS", "searchBooks page=%d response: %d", page, httpCode);

  // Pick up the total before any early returns so the count is still written
  // into `out` on partial-success paths (currently the early returns bail out
  // before populating `out` anyway, but reading the header here is cheap and
  // future-proofs the assignment).
  const String totalCountHeader = http.header("Total-Count");
  out.totalCount = totalCountHeader.length() > 0 ? totalCountHeader.toInt() : 0;

  if (httpCode < 0) {
    http.end();
    return NETWORK_ERROR;
  }
  if (httpCode == 401) {
    http.end();
    return AUTH_FAILED;
  }
  if (httpCode != 200) {
    http.end();
    return SERVER_ERROR;
  }

  BookFusionSearchJsonStream responseStream(out, page);
  const int writeResult = http.writeToStream(&responseStream);
  http.end();

  if (writeResult < 0) {
    LOG_ERR("BFS", "searchBooks body stream error: %d", writeResult);
    return NETWORK_ERROR;
  }
  if (responseStream.empty()) {
    LOG_ERR("BFS", "searchBooks response body empty");
    return JSON_ERROR;
  }
  if (!responseStream.ok()) {
    LOG_ERR("BFS", "searchBooks streaming JSON parse error after %zu bytes", responseStream.bytesRead());
    return JSON_ERROR;
  }

  // Authoritative hasMore: derived from Total-Count when present (the common
  // case — confirmed via empirical header dump). Falls back to "we got a full
  // page" only if the header is missing, since with per_page=BOOKS_PER_PAGE
  // the server no longer hands us a sentinel overflow book to detect.
  if (out.totalCount > 0) {
    out.hasMore = page * BOOKS_PER_PAGE < out.totalCount;
  } else {
    out.hasMore = out.count == BOOKS_PER_PAGE;
  }

  LOG_DBG("BFS", "searchBooks: %d books on page %d (total=%d, hasMore=%d), streamed=%zu bytes", out.count, page,
          out.totalCount, out.hasMore, responseStream.bytesRead());
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::getDownloadUrl(uint32_t bookId, char* outUrl, size_t maxLen) {
  if (!BF_TOKEN_STORE.hasToken()) return NO_TOKEN;

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/%lu/download", BASE_URL, static_cast<unsigned long>(bookId));

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST("{}");
  String responseBody = http.getString();
  http.end();

  LOG_DBG("BFS", "getDownloadUrl book=%lu response: %d", static_cast<unsigned long>(bookId), httpCode);

  if (httpCode < 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 403 || httpCode == 404) return NOT_FOUND;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  if (deserializeJson(doc, responseBody) != DeserializationError::Ok) {
    LOG_ERR("BFS", "getDownloadUrl JSON parse error");
    return JSON_ERROR;
  }

  const char* dlUrl = doc["url"] | "";
  if (dlUrl[0] == '\0') {
    LOG_ERR("BFS", "getDownloadUrl: missing url field");
    return JSON_ERROR;
  }

  strlcpy(outUrl, dlUrl, maxLen);
  LOG_DBG("BFS", "getDownloadUrl: ok");
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::searchBookshelves(BookFusionBookshelfList& out) {
  if (!BF_TOKEN_STORE.hasToken()) return NO_TOKEN;
  out.count = 0;

  // Server-side pagination — walk pages until we've fetched everything or
  // filled the local cap. Total-Count is read from the first response and
  // used as a hard bound; the per-page fall-through (short page = last page)
  // handles servers that ever omit the header. Pagination matches the books
  // endpoint pattern (Rails Kaminari/pagy convention).
  static constexpr int SHELVES_PER_PAGE = 32;
  static constexpr int MAX_PAGES = 8;  // safety: ≤8 requests ≈ 256 shelves of API hit
  int totalCount = 0;

  for (int page = 1; page <= MAX_PAGES; ++page) {
    if (out.count >= BookFusionBookshelfList::MAX_SHELVES) {
      LOG_DBG("BFS", "searchBookshelves: local cap (%d) reached after page %d", out.count, page - 1);
      break;
    }

    char url[128];
    snprintf(url, sizeof(url), "%s/api/user/bookshelves/search", BASE_URL);

    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    HTTPClient http;
    http.begin(secureClient, url);
    addAuthHeaders(http);
    http.addHeader("Content-Type", "application/json");

    static const char* kCollectedHeaders[] = {"Total-Count"};
    http.collectHeaders(kCollectedHeaders, sizeof(kCollectedHeaders) / sizeof(kCollectedHeaders[0]));

    JsonDocument reqBody;
    reqBody["page"] = page;
    reqBody["per_page"] = SHELVES_PER_PAGE;
    String bodyStr;
    serializeJson(reqBody, bodyStr);

    const int httpCode = http.POST(bodyStr);
    LOG_DBG("BFS", "searchBookshelves page=%d response: %d", page, httpCode);

    if (httpCode < 0) {
      http.end();
      return NETWORK_ERROR;
    }
    if (httpCode == 401) {
      http.end();
      return AUTH_FAILED;
    }
    if (httpCode != 200) {
      http.end();
      return SERVER_ERROR;
    }

    if (page == 1) {
      const String totalHeader = http.header("Total-Count");
      totalCount = totalHeader.length() > 0 ? totalHeader.toInt() : 0;
    }

    const int countBeforePage = out.count;
    BookFusionBookshelfJsonStream responseStream(out);
    const int writeResult = http.writeToStream(&responseStream);
    http.end();

    if (writeResult < 0) {
      LOG_ERR("BFS", "searchBookshelves body stream error: %d", writeResult);
      return NETWORK_ERROR;
    }
    if (responseStream.empty()) {
      LOG_ERR("BFS", "searchBookshelves response body empty");
      return JSON_ERROR;
    }
    if (!responseStream.ok()) {
      LOG_ERR("BFS", "searchBookshelves streaming JSON parse error after %zu bytes", responseStream.bytesRead());
      return JSON_ERROR;
    }

    const int pageCount = out.count - countBeforePage;
    LOG_DBG("BFS", "searchBookshelves: page %d added %d shelves (total now %d, server total=%d)", page, pageCount,
            out.count, totalCount);

    if (pageCount == 0) break;                  // empty response — defensive
    if (pageCount < SHELVES_PER_PAGE) break;    // short page = last page
    if (totalCount > 0 && page * SHELVES_PER_PAGE >= totalCount) break;  // header says we're done
    if (responseStream.capped()) break;         // stream hit MAX_SHELVES mid-page
  }

  if (totalCount > 0 && out.count < totalCount) {
    LOG_DBG("BFS", "searchBookshelves: capped at %d / %d shelves; extras dropped", out.count, totalCount);
  }
  LOG_DBG("BFS", "searchBookshelves: loaded %d shelves", out.count);
  return OK;
}

const char* BookFusionSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_TOKEN:
      return "Not logged in to BookFusion";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    case PENDING:
      return "Authorization pending";
    case SLOW_DOWN:
      return "Slow down polling";
    case EXPIRED:
      return "Device code expired";
    case DENIED:
      return "Authorization denied";
    default:
      return "Unknown error";
  }
}
