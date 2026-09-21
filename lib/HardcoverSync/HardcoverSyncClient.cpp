#include "HardcoverSyncClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <strings.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

#include "HardcoverBookStore.h"
#include "HardcoverTokenStore.h"

namespace {

constexpr char API_URL[] = "https://api.hardcover.app/v1/graphql";

// Hardcover's documented ceiling is 60 requests a minute. Spacing requests a
// second apart keeps a bulk push of a whole library under it without ever
// needing to parse a 429 and back off.
constexpr uint32_t MIN_REQUEST_SPACING_MS = 1000;

// Every query below selects a handful of fields, so a real response is a few
// hundred bytes. The cap bounds the buffer if the server sends something else.
constexpr size_t MAX_RESPONSE_BYTES = 8192;

// A book Hardcover couldn't match isn't searched again for this long.
constexpr uint32_t MISS_RETRY_SECONDS = 7UL * 24UL * 3600UL;

// user_books.status_id values.
constexpr int STATUS_READING = 2;
constexpr int STATUS_READ = 3;

constexpr size_t MAX_SEARCH_CANDIDATES = 5;

// Dated day replays per push. Each is a request at the one-per-second spacing,
// so this bounds a first push of a long-read book (~15s) and fits inside the
// X4 Pro's wait before deep sleep; later days follow on the next push.
constexpr size_t MAX_HISTORY_STEPS = 15;

// --- GraphQL documents ---
// Shapes follow hardcoverapp.koplugin. Kept in flash; only the selected fields
// are asked for so responses stay small enough to buffer whole.
constexpr char Q_ME[] = "{me{id account_privacy_setting_id}}";
constexpr char Q_EDITION_BY_ISBN[] =
    "query($i13:String!,$i10:String!){editions(where:{_or:[{isbn_13:{_eq:$i13}},{isbn_10:{_eq:$i10}}]},limit:1)"
    "{id pages book{id pages}}}";
constexpr char Q_SEARCH[] = "query($q:String!){search(query:$q,query_type:\"Book\",per_page:5,page:1){ids}}";
constexpr char Q_BOOKS[] =
    "query($ids:[Int!]){books(where:{id:{_in:$ids}}){id title pages default_ebook_edition{id pages}}}";
constexpr char Q_USER_BOOK[] =
    "query($b:Int!,$u:Int!){user_books(where:{book_id:{_eq:$b},user_id:{_eq:$u}},limit:1)"
    "{id status_id user_book_reads(order_by:{id:desc},limit:1){id started_at finished_at}}}";
constexpr char M_SET_STATUS[] =
    "mutation($o:UserBookCreateInput!){insert_user_book(object:$o){error user_book"
    "{id user_book_reads(order_by:{id:desc},limit:1){id started_at finished_at}}}}";
// action_at (schema: DatesReadInput.action_at, "when the reading event
// occurred") dates a replayed day; null means now.
constexpr char M_INSERT_READ[] =
    "mutation($id:Int!,$p:Int,$e:Int,$s:date,$a:timestamptz){insert_user_book_read(user_book_id:$id,"
    "user_book_read:{progress_pages:$p,edition_id:$e,started_at:$s,action_at:$a}){error user_book_read{id}}}";
constexpr char M_UPDATE_READ[] =
    "mutation($id:Int!,$p:Int,$e:Int,$s:date,$a:timestamptz){update_user_book_read(id:$id,"
    "object:{progress_pages:$p,edition_id:$e,started_at:$s,action_at:$a}){error user_book_read{id}}}";

using Error = HardcoverSyncClient::Error;

// Closes the connection when a public call returns, however it returns. Nothing
// wolfSSL allocated may outlive the call (see the header).
std::unique_ptr<freeink::SecureHttpClient> g_client;
uint32_t g_lastRequestMs = 0;

struct ConnectionScope {
  ~ConnectionScope() { g_client.reset(); }
};

// Growable body buffer. malloc/realloc rather than std::string: an allocation
// failure has to come back as an error, not a -fno-exceptions abort.
struct ResponseBuffer {
  char* data = nullptr;
  size_t len = 0;
  size_t capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool append(const uint8_t* src, const size_t n) {
    const size_t needed = len + n + 1;
    if (needed > MAX_RESPONSE_BYTES + 1) return false;
    if (needed > capacity) {
      size_t newCap = capacity > 0 ? capacity * 2 : 1024;
      while (newCap < needed) newCap *= 2;
      if (newCap > MAX_RESPONSE_BYTES + 1) newCap = MAX_RESPONSE_BYTES + 1;
      char* grown = static_cast<char*>(realloc(data, newCap));
      if (!grown) return false;
      data = grown;
      capacity = newCap;
    }
    memcpy(data + len, src, n);
    len += n;
    data[len] = '\0';
    return true;
  }
};

bool containsIgnoreCase(const char* haystack, const char* needle) {
  const size_t n = strlen(needle);
  for (const char* h = haystack; *h; ++h) {
    if (strncasecmp(h, needle, n) == 0) return true;
  }
  return false;
}

// Hardcover reports a bad or expired token as a GraphQL error in a 200 response
// ("Unable to verify token", "invalid-jwt"), not as a 401.
Error classifyErrorMessage(const char* message) {
  if (containsIgnoreCase(message, "token") || containsIgnoreCase(message, "jwt") ||
      containsIgnoreCase(message, "unauthorized")) {
    return Error::AUTH_FAILED;
  }
  if (containsIgnoreCase(message, "throttl") || containsIgnoreCase(message, "rate limit")) {
    return Error::RATE_LIMITED;
  }
  return Error::SERVER_ERROR;
}

// Ids arrive as JSON numbers, except search's `ids`, which may be strings.
int32_t jsonId(JsonVariantConst value) {
  if (value.is<const char*>()) {
    return static_cast<int32_t>(strtol(value.as<const char*>(), nullptr, 10));
  }
  return value | 0;
}

// POST one GraphQL document and parse the reply into `response`. OK means the
// transport, HTTP status and top-level `errors` were all clean; the caller still
// checks the mutation's own `error` field.
Error runQuery(const JsonDocument& request, JsonDocument& response) {
  if (!g_client) {
    g_client = makeUniqueNoThrow<freeink::SecureHttpClient>();
    if (!g_client) {
      LOG_ERR("HCS", "HTTP client allocation failed");
      return Error::NETWORK_ERROR;
    }
    // Encrypted but not server-authenticated, the project's trust model for
    // every API client (see BookFusionSyncClient.cpp).
    g_client->setInsecure();
    g_client->setReuse(true);
    g_client->setTimeout(15000);
#ifdef CROSSPOINT_VERSION
    g_client->setUserAgent("CrossPoint-Reader/" CROSSPOINT_VERSION);
#endif
  }

  const uint32_t sinceLast = millis() - g_lastRequestMs;
  if (g_lastRequestMs != 0 && sinceLast < MIN_REQUEST_SPACING_MS) {
    vTaskDelay((MIN_REQUEST_SPACING_MS - sinceLast) / portTICK_PERIOD_MS);
  }

  String body;
  serializeJson(request, body);

  if (!g_client->begin(API_URL)) {
    LOG_ERR("HCS", "HTTP begin failed");
    g_client.reset();
    return Error::NETWORK_ERROR;
  }
  // begin() clears headers, so these must follow it.
  g_client->addHeader("Content-Type", "application/json");
  g_client->addHeader("Authorization", "Bearer " + HC_TOKEN_STORE.getToken());

  ResponseBuffer resp;
  bool overflow = false;
  const freeink::SecureHttpClient::DataCallback onData = [&](const uint8_t* data, const size_t len) {
    // Only the loop task is subscribed to the task watchdog. A push also runs on
    // the boot-sync and background tasks, which are not, and feeding it from
    // there logs "esp_task_wdt_reset(): task not found" for every chunk.
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
      esp_task_wdt_reset();
    }
    if (!resp.append(data, len)) {
      overflow = true;
      return false;
    }
    return true;
  };

  LOG_DBG("HCS", "POST graphql (%u bytes, heap %u, max alloc %u)", static_cast<unsigned>(body.length()),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  // Round-trip timing, logged on every outcome. A failure that takes exactly
  // the client timeout is the server not answering; a fast failure is local.
  // Without this the two are indistinguishable in the log.
  const unsigned long requestStartMs = millis();
  const int httpCode =
      g_client->sendRequest("POST", reinterpret_cast<const uint8_t*>(body.c_str()), body.length(), onData);
  const unsigned long elapsedMs = millis() - requestStartMs;
  g_lastRequestMs = millis();
  LOG_DBG("HCS", "graphql reply: code %d in %lu ms, %u body bytes", httpCode, elapsedMs,
          static_cast<unsigned>(resp.len));

  if (httpCode < 0) {
    // Stage + partial bytes are what separate "the server never answered" from
    // "it answered and we mis-framed it" -- indistinguishable in the old log,
    // which only ever said "-1".
    const auto& diag = g_client->lastFailure();
    LOG_ERR("HCS", "Request failed: %d after %lu ms (heap %u, max alloc %u)", httpCode, elapsedMs,
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    LOG_ERR("HCS", "  stage=%s elapsed=%lums partial=%u bytes '%s' available=%d connected=%d reused=%d", diag.stage,
            diag.elapsedMs, static_cast<unsigned>(diag.partialBytes), diag.partial, diag.available,
            static_cast<int>(diag.connected), static_cast<int>(diag.reusedConnection));
    g_client.reset();
    return Error::NETWORK_ERROR;
  }
  if (overflow) {
    LOG_ERR("HCS", "Response exceeded %u bytes or buffer allocation failed", static_cast<unsigned>(MAX_RESPONSE_BYTES));
    g_client.reset();
    return Error::JSON_ERROR;
  }
  if (httpCode == 401 || httpCode == 403) return Error::AUTH_FAILED;
  if (httpCode == 429) return Error::RATE_LIMITED;
  if (httpCode != 200) {
    LOG_ERR("HCS", "HTTP %d: %.120s", httpCode, resp.data ? resp.data : "");
    return Error::SERVER_ERROR;
  }

  // const input: ArduinoJson copies the strings it keeps, so the parsed document
  // stays valid after `resp` is freed on return.
  if (!resp.data ||
      deserializeJson(response, static_cast<const char*>(resp.data), resp.len) != DeserializationError::Ok) {
    LOG_ERR("HCS", "Response JSON parse failed");
    return Error::JSON_ERROR;
  }

  JsonArrayConst errors = response["errors"];
  if (!errors.isNull() && errors.size() > 0) {
    const char* message = errors[0]["message"] | "";
    LOG_ERR("HCS", "GraphQL error: %s", message);
    return classifyErrorMessage(message);
  }
  if (const char* message = response["error"] | static_cast<const char*>(nullptr)) {
    LOG_ERR("HCS", "API error: %s", message);
    return classifyErrorMessage(message);
  }
  return Error::OK;
}

// A mutation reports business-rule failures in its own `error` field.
bool mutationFailed(JsonVariantConst result, const char* name) {
  if (result.isNull()) {
    LOG_ERR("HCS", "%s returned nothing", name);
    return true;
  }
  if (const char* message = result["error"] | static_cast<const char*>(nullptr)) {
    LOG_ERR("HCS", "%s error: %s", name, message);
    return true;
  }
  return false;
}

// Caches the account id and default privacy that pushes need.
Error queryAccount() {
  JsonDocument request;
  request["query"] = Q_ME;
  JsonDocument response;
  const Error err = runQuery(request, response);
  if (err != Error::OK) return err;

  // `me` is a one-element list; empty means the token maps to no account.
  JsonVariantConst me = response["data"]["me"][0];
  const int32_t userId = me["id"] | 0;
  if (userId == 0) {
    LOG_ERR("HCS", "Token accepted but no account returned");
    return Error::AUTH_FAILED;
  }
  HC_TOKEN_STORE.setAccount(userId, static_cast<uint8_t>(me["account_privacy_setting_id"] | 1));
  LOG_DBG("HCS", "Hardcover account %d", static_cast<int>(userId));
  return Error::OK;
}

// ISBN-10 for a 978-prefixed ISBN-13; for 979 (no ISBN-10 exists) the 13 is
// returned unchanged, which can't match any isbn_10 value.
void isbn10From13(const char* isbn13, char* out10) {
  if (strncmp(isbn13, "978", 3) != 0) {
    strlcpy(out10, isbn13, 14);
    return;
  }
  int sum = 0;
  for (int i = 0; i < 9; i++) {
    out10[i] = isbn13[3 + i];
    sum += (isbn13[3 + i] - '0') * (10 - i);
  }
  const int check = (11 - sum % 11) % 11;
  out10[9] = check == 10 ? 'X' : static_cast<char>('0' + check);
  out10[10] = '\0';
}

bool readIsbn(const char* isbnPath, char* out13) {
  if (!isbnPath || !Storage.exists(isbnPath)) return false;
  String text = Storage.readFile(isbnPath);
  text.trim();
  if (text.length() != 13) return false;
  for (unsigned i = 0; i < 13; i++) {
    if (!isdigit(static_cast<unsigned char>(text[i]))) return false;
  }
  strlcpy(out13, text.c_str(), 14);
  return true;
}

Error linkByIsbn(const char* isbn13, HardcoverBookLink& link) {
  char isbn10[14];
  isbn10From13(isbn13, isbn10);

  JsonDocument request;
  request["query"] = Q_EDITION_BY_ISBN;
  request["variables"]["i13"] = isbn13;
  request["variables"]["i10"] = isbn10;
  JsonDocument response;
  const Error err = runQuery(request, response);
  if (err != Error::OK) return err;

  JsonVariantConst edition = response["data"]["editions"][0];
  const int32_t bookId = edition["book"]["id"] | 0;
  if (bookId == 0) return Error::NOT_FOUND;

  link.bookId = bookId;
  link.editionId = edition["id"] | 0;
  link.pages = edition["pages"] | 0;
  if (link.pages <= 0) link.pages = edition["book"]["pages"] | 0;
  LOG_INF("HCS", "Linked by ISBN %s: book %d edition %d (%d pages)", isbn13, static_cast<int>(link.bookId),
          static_cast<int>(link.editionId), static_cast<int>(link.pages));
  return Error::OK;
}

// Title key for matching: lowercase ASCII letters and digits, non-ASCII bytes
// kept as-is, everything else dropped, cut at a subtitle colon.
// "The Hobbit: or There and Back Again" -> "thehobbit".
void titleKey(const char* title, char* out, const size_t outLen) {
  size_t n = 0;
  for (const char* p = title; *p && *p != ':' && n + 1 < outLen; ++p) {
    const auto c = static_cast<unsigned char>(*p);
    if (c >= 0x80) {
      out[n++] = static_cast<char>(c);
    } else if (isalnum(c)) {
      out[n++] = static_cast<char>(tolower(c));
    }
  }
  out[n] = '\0';
}

// Search by title + first author, then accept the first candidate (in search
// rank order) whose title matches the EPUB's. Fuzzy search alone would link a
// book to whatever ranked first, and a wrong link silently logs progress
// against someone else's book.
Error linkByTitle(const char* title, const char* author, HardcoverBookLink& link) {
  if (!title || title[0] == '\0') return Error::NOT_FOUND;

  char wanted[64];
  titleKey(title, wanted, sizeof(wanted));
  if (wanted[0] == '\0') return Error::NOT_FOUND;

  // Subtitle dropped; only the first of several " & "-joined authors.
  const char* colon = strchr(title, ':');
  const int titleLen = colon ? static_cast<int>(colon - title) : static_cast<int>(strlen(title));
  const char* amp = author ? strstr(author, " & ") : nullptr;
  const int authorLen = !author ? 0 : (amp ? static_cast<int>(amp - author) : static_cast<int>(strlen(author)));
  char searchText[128];
  snprintf(searchText, sizeof(searchText), "%.*s %.*s", titleLen, title, authorLen, author ? author : "");

  int32_t candidates[MAX_SEARCH_CANDIDATES] = {};
  size_t candidateCount = 0;
  {
    JsonDocument request;
    request["query"] = Q_SEARCH;
    request["variables"]["q"] = searchText;
    JsonDocument response;
    const Error err = runQuery(request, response);
    if (err != Error::OK) return err;
    for (JsonVariantConst id : response["data"]["search"]["ids"].as<JsonArrayConst>()) {
      if (candidateCount >= MAX_SEARCH_CANDIDATES) break;
      const int32_t value = jsonId(id);
      if (value > 0) candidates[candidateCount++] = value;
    }
  }
  if (candidateCount == 0) return Error::NOT_FOUND;

  JsonDocument request;
  request["query"] = Q_BOOKS;
  JsonArray ids = request["variables"]["ids"].to<JsonArray>();
  for (size_t i = 0; i < candidateCount; i++) ids.add(candidates[i]);
  JsonDocument response;
  const Error err = runQuery(request, response);
  if (err != Error::OK) return err;

  JsonArrayConst books = response["data"]["books"];
  for (size_t i = 0; i < candidateCount; i++) {
    for (JsonVariantConst book : books) {
      if (jsonId(book["id"]) != candidates[i]) continue;
      char found[64];
      titleKey(book["title"] | "", found, sizeof(found));
      if (strcmp(found, wanted) != 0) break;

      link.bookId = candidates[i];
      link.editionId = book["default_ebook_edition"]["id"] | 0;
      link.pages = book["default_ebook_edition"]["pages"] | 0;
      if (link.pages <= 0) link.pages = book["pages"] | 0;
      LOG_INF("HCS", "Linked by title \"%s\": book %d edition %d (%d pages)", searchText, static_cast<int>(link.bookId),
              static_cast<int>(link.editionId), static_cast<int>(link.pages));
      return Error::OK;
    }
  }
  LOG_INF("HCS", "No title match among %u results for \"%s\"", static_cast<unsigned>(candidateCount), searchText);
  return Error::NOT_FOUND;
}

// An unlinked book whose last match attempt failed within the retry window.
bool recentlyMissed(const HardcoverBookLink& link) {
  if (link.bookId != 0 || link.lastMissEpoch == 0) return false;
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  return now >= link.lastMissEpoch && now - link.lastMissEpoch < MISS_RETRY_SECONDS;
}

Error ensureLinked(const HardcoverBookInfo& book, HardcoverBookLink& link) {
  if (link.bookId != 0) return Error::OK;

  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  Error err = Error::NOT_FOUND;
  char isbn13[14];
  if (readIsbn(book.isbnPath, isbn13)) {
    err = linkByIsbn(isbn13, link);
  }
  if (err == Error::NOT_FOUND) {
    err = linkByTitle(book.title, book.author, link);
  }
  if (err == Error::NOT_FOUND) {
    link.lastMissEpoch = now;
  }
  return err;
}

// Reads the user_book's id and its newest read into `link`; a read is only kept
// if it is still open (no finished_at).
void readUserBook(JsonVariantConst userBook, HardcoverBookLink& link) {
  link.userBookId = userBook["id"] | 0;
  link.readId = 0;
  link.startedAt[0] = '\0';
  JsonVariantConst read = userBook["user_book_reads"][0];
  if (!read.isNull() && read["finished_at"].isNull()) {
    link.readId = read["id"] | 0;
    strlcpy(link.startedAt, read["started_at"] | "", sizeof(link.startedAt));
  }
}

Error lookupUserBook(HardcoverBookLink& link, int& outStatus) {
  JsonDocument request;
  request["query"] = Q_USER_BOOK;
  request["variables"]["b"] = link.bookId;
  request["variables"]["u"] = HC_TOKEN_STORE.getUserId();
  JsonDocument response;
  const Error err = runQuery(request, response);
  if (err != Error::OK) return err;

  JsonVariantConst userBook = response["data"]["user_books"][0];
  if (userBook.isNull()) {
    link.userBookId = 0;
    link.readId = 0;
    link.startedAt[0] = '\0';
    outStatus = 0;
    return Error::OK;
  }
  readUserBook(userBook, link);
  outStatus = userBook["status_id"] | 0;
  return Error::OK;
}

// insert_user_book adds the book or, if it's already on a shelf, changes its
// status — the plugin uses it for both.
Error setStatus(HardcoverBookLink& link, const int status) {
  JsonDocument request;
  request["query"] = M_SET_STATUS;
  JsonObject object = request["variables"]["o"].to<JsonObject>();
  object["book_id"] = link.bookId;
  object["status_id"] = status;
  object["privacy_setting_id"] = HC_TOKEN_STORE.getPrivacySettingId();
  if (link.editionId != 0) object["edition_id"] = link.editionId;
  JsonDocument response;
  const Error err = runQuery(request, response);
  if (err != Error::OK) return err;

  JsonVariantConst result = response["data"]["insert_user_book"];
  if (mutationFailed(result, "insert_user_book")) return Error::SERVER_ERROR;
  JsonVariantConst userBook = result["user_book"];
  if (!userBook.isNull()) {
    readUserBook(userBook, link);
  }
  return Error::OK;
}

// One progress write in a push: a dated replay of a past day, or the current
// position. 16 bytes; the dates point into the caller's HardcoverBookInfo.
struct ProgressStep {
  int32_t pages = 0;
  float percent = 0.0f;
  const char* startDate = nullptr;  // YYYY-MM-DD this step would start a read on
  bool dated = false;               // replay of startDate's day; false = now
};

void setReadVariables(JsonObject vars, const HardcoverBookLink& link, const ProgressStep& step) {
  if (link.pages > 0) {
    vars["p"] = step.pages;
  } else {
    vars["p"] = nullptr;
  }
  if (link.editionId != 0) {
    vars["e"] = link.editionId;
  } else {
    vars["e"] = nullptr;
  }
  if (step.dated && step.startDate) {
    // Midday UTC keeps the timestamp on the same calendar date for any
    // timezone within +/-11h, which is all a day-granular replay needs.
    // ArduinoJson copies a char* value, so the local buffer may go.
    char actionAt[24];
    snprintf(actionAt, sizeof(actionAt), "%.10sT12:00:00Z", step.startDate);
    vars["a"] = actionAt;
  } else {
    vars["a"] = nullptr;
  }
}

Error insertRead(HardcoverBookLink& link, const ProgressStep& step) {
  JsonDocument request;
  request["query"] = M_INSERT_READ;
  JsonObject vars = request["variables"].to<JsonObject>();
  vars["id"] = link.userBookId;
  setReadVariables(vars, link, step);
  const bool hasStart = step.startDate && step.startDate[0] != '\0';
  if (hasStart) {
    vars["s"] = step.startDate;
  } else {
    vars["s"] = nullptr;
  }
  JsonDocument response;
  const Error err = runQuery(request, response);
  if (err != Error::OK) return err;

  JsonVariantConst result = response["data"]["insert_user_book_read"];
  if (mutationFailed(result, "insert_user_book_read")) return Error::SERVER_ERROR;
  link.readId = result["user_book_read"]["id"] | 0;
  strlcpy(link.startedAt, hasStart ? step.startDate : "", sizeof(link.startedAt));
  return Error::OK;
}

// started_at is sent back unchanged: the plugin always passes it, which suggests
// the update replaces the date fields rather than merging them.
Error updateRead(HardcoverBookLink& link, const ProgressStep& step, bool& outStale) {
  JsonDocument request;
  request["query"] = M_UPDATE_READ;
  JsonObject vars = request["variables"].to<JsonObject>();
  vars["id"] = link.readId;
  setReadVariables(vars, link, step);
  if (link.startedAt[0] != '\0') {
    vars["s"] = link.startedAt;
  } else {
    vars["s"] = nullptr;
  }
  JsonDocument response;
  const Error err = runQuery(request, response);
  if (err != Error::OK) return err;

  // A read deleted on the website (or one belonging to another account after a
  // token change) comes back as an error or an empty result. Treat both as
  // stale cached ids rather than a failure.
  JsonVariantConst result = response["data"]["update_user_book_read"];
  if (mutationFailed(result, "update_user_book_read") || result["user_book_read"].isNull()) {
    outStale = true;
  }
  return Error::OK;
}

// Writes `steps` in order: the first starts a read if there is no open one,
// the rest update it. The last step is always the current position, so the
// read ends on it however many dated replays came before.
Error pushOnce(HardcoverBookLink& link, const ProgressStep* steps, const size_t stepCount, const bool finished,
               bool& outStale) {
  outStale = false;

  if (link.userBookId == 0) {
    int status = 0;
    Error err = lookupUserBook(link, status);
    if (err != Error::OK) return err;
    if (finished && status == STATUS_READ) {
      return Error::OK;  // already Read on Hardcover
    }
    // Missing from the shelves, or shelved as Want to Read / Read / DNF:
    // reading it on the device makes it Currently Reading.
    if (link.userBookId == 0 || status != STATUS_READING) {
      err = setStatus(link, STATUS_READING);
      if (err != Error::OK) return err;
    }
  }

  for (size_t i = 0; i < stepCount; i++) {
    Error err = Error::OK;
    if (link.readId == 0) {
      err = insertRead(link, steps[i]);
    } else {
      err = updateRead(link, steps[i], outStale);
      if (outStale) return Error::OK;
    }
    if (err != Error::OK) return err;
  }

  return finished ? setStatus(link, STATUS_READ) : Error::OK;
}

int32_t pagesFor(const int32_t totalPages, const float percent, const bool finished) {
  if (totalPages <= 0) return 0;
  if (finished) return totalPages;
  const long pages = lround(static_cast<double>(percent) * totalPages / 100.0);
  if (pages < 0) return 0;
  return pages > totalPages ? totalPages : static_cast<int32_t>(pages);
}

bool linkIsUpToDate(const HardcoverBookLink& link, const float percent, const bool finished) {
  if (link.bookId == 0 || link.userBookId == 0) return false;
  if (finished) return link.finished;
  if (link.finished) return true;  // Read on Hardcover; don't reopen it from a stale position
  if (link.pages > 0) return link.syncedPages == pagesFor(link.pages, percent, false);
  return fabs(link.syncedPercent - percent) < 1.0f;
}

// Dated replays of the days Hardcover hasn't had yet, oldest first. Today is
// left to the current-position step. Replays only move forward: a day at or
// behind the previous write (a re-read, or a jump back) is skipped. Returns the
// count written to `out`; `outCapped` is set when older days had to wait.
size_t buildHistorySteps(const HardcoverBookInfo& book, const HardcoverBookLink& link, ProgressStep* out,
                         const size_t maxSteps, uint32_t& outLastDay, bool& outCapped) {
  size_t count = 0;
  int32_t lastPages = link.syncedPages;
  float lastPercent = link.syncedPercent;
  outLastDay = 0;
  outCapped = false;
  for (size_t i = 0; i < book.dayCount; i++) {
    const HardcoverDayProgress& day = book.days[i];
    if (day.dayOrdinal <= link.syncedThroughDay) continue;
    if (book.todayOrdinal != 0 && day.dayOrdinal >= book.todayOrdinal) break;
    const float percent = static_cast<float>(day.percent);
    const int32_t pages = pagesFor(link.pages, percent, false);
    const bool forward = link.pages > 0 ? pages > lastPages : percent > lastPercent;
    if (!forward) continue;
    if (count == maxSteps) {
      outCapped = true;
      break;
    }
    ProgressStep& step = out[count++];
    step.pages = pages;
    step.percent = percent;
    step.startDate = day.date;
    step.dated = true;
    lastPages = pages;
    lastPercent = percent;
    outLastDay = day.dayOrdinal;
  }
  return count;
}

}  // namespace

bool HardcoverSyncClient::isUpToDate(const char* epubPath, const float percent, const bool finished) {
  HardcoverBookLink link;
  if (!HardcoverBookStore::load(epubPath, link)) return false;
  // A book Hardcover couldn't match has nothing to push until the retry window
  // lapses; reporting it as pending would bring WiFi up only to hit the cache.
  return recentlyMissed(link) || linkIsUpToDate(link, percent, finished);
}

void HardcoverSyncClient::forgetSyncState(const char* epubPath) {
  HardcoverBookLink link;
  if (!HardcoverBookStore::load(epubPath, link)) return;
  link.userBookId = 0;
  link.readId = 0;
  link.startedAt[0] = '\0';
  link.syncedPages = -1;
  link.syncedPercent = 0.0f;
  link.syncedThroughDay = 0;
  link.finished = false;
  link.lastMissEpoch = 0;
  HardcoverBookStore::save(epubPath, link);
}

HardcoverSyncClient::Error HardcoverSyncClient::pushProgress(const HardcoverBookInfo& book, const float percent,
                                                             const bool finished, const char* todayIso,
                                                             HardcoverPushResult* out) {
  if (!HC_TOKEN_STORE.hasToken()) return NO_TOKEN;
  if (!book.epubPath) return NOT_FOUND;

  HardcoverBookLink link;
  HardcoverBookStore::load(book.epubPath, link);
  if (recentlyMissed(link)) {
    LOG_DBG("HCS", "Skipping recently unmatched book: %s", book.title ? book.title : "");
    return NOT_FOUND;
  }
  if (linkIsUpToDate(link, percent, finished)) {
    if (out) {
      out->pages = link.syncedPages > 0 ? link.syncedPages : 0;
      out->totalPages = link.pages;
      out->markedRead = link.finished;
    }
    return OK;
  }

  ConnectionScope scope;
  Error err = OK;
  if (HC_TOKEN_STORE.getUserId() == 0) {
    err = queryAccount();
    if (err != OK) return err;
  }

  const uint32_t missBefore = link.lastMissEpoch;
  err = ensureLinked(book, link);
  if (err != OK) {
    if (link.lastMissEpoch != missBefore) {
      HardcoverBookStore::save(book.epubPath, link);  // remember the miss
    }
    return err;
  }

  // Replayed days, then the current position: (MAX_HISTORY_STEPS + 1) * 16
  // bytes = 256 on the stack.
  ProgressStep steps[MAX_HISTORY_STEPS + 1];
  uint32_t lastHistoryDay = 0;
  bool historyCapped = false;
  size_t stepCount = buildHistorySteps(book, link, steps, MAX_HISTORY_STEPS, lastHistoryDay, historyCapped);
  const int32_t pages = pagesFor(link.pages, percent, finished);
  ProgressStep& current = steps[stepCount++];
  current.pages = pages;
  current.percent = percent;
  current.startDate = stepCount > 1 ? steps[0].startDate : todayIso;

  // A second attempt only runs when the cached user_book/read ids turned out
  // to be stale; it re-reads them from the server first.
  bool stale = false;
  for (int attempt = 0; attempt < 2; attempt++) {
    err = pushOnce(link, steps, stepCount, finished, stale);
    if (err != OK || !stale) break;
    LOG_INF("HCS", "Cached Hardcover ids were stale; looking them up again");
    link.userBookId = 0;
    link.readId = 0;
    link.startedAt[0] = '\0';
  }
  if (err == OK && stale) {
    err = SERVER_ERROR;
  }

  if (err == OK) {
    link.syncedPages = pages;
    link.syncedPercent = finished ? 100.0f : percent;
    link.finished = finished;
    // Capped: resume after the last replayed day next time. Otherwise every
    // day up to today is covered (today by the current-position step).
    if (historyCapped) {
      link.syncedThroughDay = lastHistoryDay;
    } else if (book.todayOrdinal != 0) {
      link.syncedThroughDay = book.todayOrdinal;
    } else if (lastHistoryDay != 0) {
      link.syncedThroughDay = lastHistoryDay;
    }
    if (out) {
      out->pages = pages;
      out->totalPages = link.pages;
      out->markedRead = finished;
    }
    LOG_INF("HCS", "Pushed %.0f%% (%d/%d pages) after %u dated day(s)%s%s", percent, static_cast<int>(pages),
            static_cast<int>(link.pages), static_cast<unsigned>(stepCount - 1), historyCapped ? ", more pending" : "",
            finished ? ", marked Read" : "");
  }
  // Save even on failure: ids learned before the failing step spare the retry
  // those requests.
  HardcoverBookStore::save(book.epubPath, link);
  return err;
}

const char* HardcoverSyncClient::errorString(const Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_TOKEN:
      return "No Hardcover token";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Token rejected";
    case RATE_LIMITED:
      return "Rate limited";
    case SERVER_ERROR:
      return "Server error";
    case JSON_ERROR:
      return "Bad response";
    case NOT_FOUND:
      return "Book not found on Hardcover";
  }
  return "Unknown error";
}
