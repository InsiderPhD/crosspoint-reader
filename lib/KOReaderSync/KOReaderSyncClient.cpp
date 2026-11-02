#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <BookFusionBookIdStore.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <SecureHttpClient.h>
#include <base64.h>
#include <esp_mac.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
// Shared legacy device_id; strict KOSync servers still get it (deviceIdForServer).
constexpr char DEVICE_ID[] = "crosspoint-reader";

// Transport is freeink::SecureHttpClient (wolfSSL, TLS 1.3), like every other
// API client in this fork. The precompiled mbedTLS esp_http_client path this
// replaced needs two ~16.7KB contiguous record buffers for the handshake (see
// platformio.ini), which fails once a book is open and surfaces as a bare
// "Network error". setInsecure(): encrypted but not server-authenticated, the
// project's trust model for every API client (see BookFusionSyncClient.cpp).
//
// Preflight margins, not a guarantee that a handshake will fit: a free-space
// floor plus room for a full 16KB TLS record when the server does not
// negotiate a smaller record limit. Values follow upstream (#3412).
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 15000;

// True when free heap is too low to risk a TLS handshake.
bool insufficientHeap() {
  const uint32_t freeBytes = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  if (freeBytes < MIN_FREE_FOR_TLS || largestBlock < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u free (need %u), %u max alloc (need %u)",
            (unsigned)freeBytes, (unsigned)MIN_FREE_FOR_TLS, (unsigned)largestBlock, (unsigned)MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

// begin() the request and apply the KOSync auth headers (begin() clears
// headers, so they must follow it). x-auth-* is the native KOSync scheme; Basic
// auth is added for Calibre-Web-Automated compatibility.
bool beginRequest(freeink::SecureHttpClient& http, const std::string& url) {
  http.setInsecure();
  http.setReuse(false);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return false;
  }
  // Caller-configured headers (e.g. a Cloudflare Access service token) go
  // first, so the KOSync auth scheme below always wins a name clash.
  for (const auto& header : KOREADER_STORE.getCustomHeaders()) {
    if (!header.name.empty()) http.addHeader(header.name, header.value);
  }
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  const std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  const String encoded = base64::encode(reinterpret_cast<const uint8_t*>(credentials.data()), credentials.size());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
  return true;
}
}  // namespace

std::optional<KOReaderMetadata> KOReaderMetadata::fromBook(const Epub& epub) {
  if (!KOREADER_STORE.getSendMetadata()) {
    return std::nullopt;
  }

  KOReaderMetadata meta;
  const std::string& path = epub.getPath();
  const auto lastSlash = path.rfind('/');
  meta.filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
  meta.title = epub.getTitle();
  meta.authors = epub.getAuthor();

  if (KOREADER_STORE.supportsExtendedMetadata()) {
    meta.series = epub.getSeriesName();
    const std::string index = epub.getSeriesIndex();
    if (!index.empty()) {
      char* end = nullptr;
      const float value = strtof(index.c_str(), &end);
      if (end != index.c_str()) {
        meta.seriesIndex = value;
      }
    }
    // ASIN sidecar, same lifecycle as the ISBN one below.
    const std::string asinPath = epub.getAsinPath();
    if (Storage.exists(asinPath.c_str())) {
      const String asin = Storage.readFile(asinPath.c_str());
      bool valid = asin.length() == 10;
      for (unsigned int i = 0; valid && i < asin.length(); i++) {
        valid = isalnum(static_cast<unsigned char>(asin[i])) != 0;
      }
      if (valid) {
        meta.asin = asin.c_str();
      }
    }
    // ISBN-13 sidecar written when the book's metadata was cached; absent for a
    // book with no valid ISBN. Anything that is not 13 digits is not sent.
    const std::string isbnPath = epub.getIsbnPath();
    if (Storage.exists(isbnPath.c_str())) {
      const String isbn = Storage.readFile(isbnPath.c_str());
      bool valid = isbn.length() == 13;
      for (unsigned int i = 0; valid && i < isbn.length(); i++) {
        valid = isbn[i] >= '0' && isbn[i] <= '9';
      }
      if (valid) {
        meta.isbn = isbn.c_str();
      }
    }
  }

  // Service ids: the plugin sidecar a catalog download left next to the book
  // (missing in the common case), plus our native BookFusion download id.
  const std::string sidecarPath = path + ".meta.json";
  if (Storage.exists(sidecarPath.c_str())) {
    Storage.readFileToString("KOSync", sidecarPath, 2 * 1024, meta.extraJson);
  }
  const uint32_t bookFusionId = BookFusionBookIdStore::loadBookId(path.c_str());
  if (bookFusionId != 0) {
    JsonDocument extra;
    if (meta.extraJson.empty() || deserializeJson(extra, meta.extraJson) != DeserializationError::Ok ||
        !extra.is<JsonObject>()) {
      extra.to<JsonObject>();
    }
    if (extra["bookfusion_id"].isNull()) {
      extra["bookfusion_id"] = std::to_string(bookFusionId);
      meta.extraJson.clear();
      serializeJson(extra, meta.extraJson);
    }
  }
  return meta;
}

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  freeink::SecureHttpClient http;
  if (!beginRequest(http, url)) return NETWORK_ERROR;
  const int httpCode = http.GET();
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  // Any 2xx is success: KOSync-compatible servers differ (BookLore/grimmory is
  // a Spring service and uses the idiomatic codes) — upstream #2876.
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_DBG("KOSync", "Creating account: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  // KOSync stores the MD5 of the password, the same value x-auth-key sends.
  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  freeink::SecureHttpClient http;
  if (!beginRequest(http, url)) return NETWORK_ERROR;
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Create user response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode >= 200 && httpCode < 300) return OK;
  // The reference server answers 402 for a username that is already registered.
  if (httpCode == 402) return USER_EXISTS;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  // Advertise percentage-only positions: servers such as Calibre-Web NextGen withhold a
  // record that has no locator unless the client says it can take a bare percentage.
  std::string url =
      KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash + "?position_kinds=locator,percentage";
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  freeink::SecureHttpClient http;
  if (!beginRequest(http, url)) return NETWORK_ERROR;
  const int httpCode = http.GET();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode <= 0) {
    http.end();
    return NETWORK_ERROR;
  }

  // 204 = no stored progress for this document (Spring-style servers; the
  // reference server answers 200 with an empty object). Same path as 404.
  if (httpCode == 204) {
    http.end();
    return NOT_FOUND;
  }

  if (httpCode >= 200 && httpCode < 300) {
    JsonDocument doc;
    const auto body = http.getString();
    const DeserializationError error = deserializeJson(doc, body.c_str());
    http.end();

    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    outProgress.position.reset();
    if (KOREADER_STORE.supportsRichProgress()) {
      const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
      if (!pos.isNull()) {
        KOReaderRichPosition rich;
        rich.pctQ = pos["pctQ"].as<uint32_t>();
        rich.spineIndex = pos["spine"].as<uint16_t>();
        rich.pageNumber = pos["page"].as<uint16_t>();
        const uint16_t pages = pos["pages"].as<uint16_t>();
        rich.totalPages = pages > 0 ? pages : 1;
        const uint16_t para = pos["para"].as<uint16_t>();
        if (para > 0) rich.paragraphIndex = para;
        rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
        LOG_DBG("KOSync", "Got rich position: spine=%u page=%u/%u para=%u", rich.spineIndex, rich.pageNumber,
                rich.totalPages, para);
        outProgress.position = std::move(rich);
      }
    }

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  // A guessed /body/p[N] can send KOReader to the start of the book. Better to
  // upload nothing than a position that moves the reader backwards.
  if (progress.progressIsGuess && !KOREADER_STORE.supportsRichProgress()) {
    LOG_ERR("KOSync", "Not uploading a guessed XPath to a KOSync server: %s", progress.progress.c_str());
    return POSITION_UNRESOLVED;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  fillProgressJson(doc.to<JsonObject>(), progress);

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  freeink::SecureHttpClient http;
  if (!beginRequest(http, url)) return NETWORK_ERROR;
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("PUT", body);
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  // Any 2xx accepts the progress: Spring-based servers answer a PUT with
  // 201/204 (upstream #2876).
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

void KOReaderSyncClient::fillProgressJson(JsonObject doc, const KOReaderProgress& progress) {
  doc["document"] = progress.document;
  // The store is checked again here so no caller can leak metadata past the
  // user's switch, or the extended fields to a strict KOSync server.
  if (progress.metadata.has_value() && KOREADER_STORE.getSendMetadata()) {
    const KOReaderMetadata& m = *progress.metadata;
    auto meta = doc["metadata"].to<JsonObject>();
    meta["filename"] = m.filename;
    meta["title"] = m.title;
    meta["authors"] = m.authors;
    if (KOREADER_STORE.supportsExtendedMetadata()) {
      if (!m.isbn.empty()) meta["isbn"] = m.isbn;
      if (!m.asin.empty()) meta["asin"] = m.asin;
      if (!m.series.empty()) meta["series"] = m.series;
      if (m.seriesIndex.has_value()) meta["series_index"] = *m.seriesIndex;
    }
    JsonDocument extra;
    if (!m.extraJson.empty() && deserializeJson(extra, m.extraJson) == DeserializationError::Ok) {
      for (JsonPairConst kv : extra.as<JsonObjectConst>()) {
        // Flat strings, numbers and booleans keep their JSON type; null and
        // nested values are skipped, and the reserved keys above always win.
        const JsonVariantConst value = kv.value();
        if (!(value.is<const char*>() || value.is<bool>() || value.is<long long>() || value.is<double>())) continue;
        if (!meta[kv.key().c_str()].isNull()) continue;
        meta[kv.key().c_str()] = value;
      }
    }
  }
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  char deviceId[READER_ID_SIZE];
  deviceIdForServer(deviceId, sizeof(deviceId));
  doc["device_id"] = deviceId;
  if (progress.position.has_value() && KOREADER_STORE.supportsRichProgress()) {
    // Opt-in by server type; strict KOSync never receives it.
    const KOReaderRichPosition& p = *progress.position;
    auto pos = doc["position"].to<JsonObject>();
    pos["pctQ"] = p.pctQ;
    pos["spine"] = p.spineIndex;
    pos["page"] = p.pageNumber;
    pos["pages"] = p.totalPages;
    if (p.paragraphIndex.has_value()) pos["para"] = *p.paragraphIndex;
    // The server rejects the whole position object if xpath exceeds 120 bytes.
    if (!p.xpath.empty() && p.xpath.size() <= 120) pos["xpath"] = p.xpath;
  }
}

bool KOReaderProgressBatch::add(const KOReaderProgress& progress) {
  if (count_ >= KOReaderSyncClient::MAX_PROGRESS_BATCH) return false;
  if (progress.document.size() != 32) return false;
  // A batch only ever goes to a CrossPoint/Other server, whose receivers fall
  // back to the rich position, so a guessed XPath is acceptable here (the
  // same rule updateProgress applies).
  JsonDocument item;
  KOReaderSyncClient::fillProgressJson(item.to<JsonObject>(), progress);
  if (count_ == 0) {
    // Typical item ~250-450 bytes (XPath capped at 120 in the rich position,
    // metadata only with Send Book Details); one reservation for the batch so
    // appending items never reallocates mid-run.
    body_.reserve(KOReaderSyncClient::MAX_PROGRESS_BATCH * 512);
    body_ = "{\"items\":[";
  } else {
    body_ += ',';
  }
  // serializeJson() replaces a std::string's contents, so serialize the item
  // on its own (a few hundred bytes, freed at return) and append it.
  std::string json;
  serializeJson(item, json);
  body_ += json;
  ++count_;
  return true;
}

void KOReaderProgressBatch::clear() {
  body_.clear();
  count_ = 0;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgressBatch(KOReaderProgressBatch& batch) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (!KOREADER_STORE.supportsSyncExtensions()) return BATCH_UNSUPPORTED;
  if (batch.count() == 0) return OK;
  if (insufficientHeap()) return LOW_MEMORY;

  batch.body_ += "]}";
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/progress/batch";
  LOG_DBG("KOSync", "Batch upload of %u positions (%u bytes, heap %u)", static_cast<unsigned>(batch.count()),
          static_cast<unsigned>(batch.body_.size()), (unsigned)ESP.getFreeHeap());

  freeink::SecureHttpClient http;
  if (!beginRequest(http, url)) return NETWORK_ERROR;
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("PUT", batch.body_);
  http.end();
  lastHttpCode = httpCode;
  LOG_DBG("KOSync", "Batch upload response: %d", httpCode);
  // The request body is gone either way; the caller re-adds on a retry.
  batch.clear();

  if (httpCode <= 0) return NETWORK_ERROR;
  // All or nothing on the server, so any 2xx means every item was stored.
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  // No route: an older CrossPoint Sync or a custom server. One PUT per book.
  if (httpCode == 404 || httpCode == 405) return BATCH_UNSUPPORTED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgressChanges(const uint32_t since, const uint8_t limit,
                                                                 ChangesPage& page,
                                                                 std::vector<KOReaderProgressChange>& out) {
  lastHttpCode = 0;
  out.clear();
  page = {};
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (!KOREADER_STORE.supportsSyncExtensions()) return FEED_UNSUPPORTED;
  if (insufficientHeap()) return LOW_MEMORY;
  // The page body (server cap 8KB) is buffered whole before parsing, so ask
  // for room for it on top of the handshake.
  if (ESP.getMaxAllocHeap() < MIN_BLOCK_FOR_TLS + MAX_CHANGES_BODY) {
    LOG_ERR("KOSync", "Change feed needs a %u-byte block, largest is %u",
            static_cast<unsigned>(MIN_BLOCK_FOR_TLS + MAX_CHANGES_BODY), ESP.getMaxAllocHeap());
    return LOW_MEMORY;
  }

  char deviceId[READER_ID_SIZE];
  deviceIdForServer(deviceId, sizeof(deviceId));
  char query[96];
  snprintf(query, sizeof(query), "/api/v1/progress/changes?since=%lu&limit=%u&device=%s",
           static_cast<unsigned long>(since), static_cast<unsigned>(std::min<uint8_t>(limit, MAX_CHANGES_PAGE)),
           deviceId);
  const std::string url = KOREADER_STORE.getBaseUrl() + query;
  LOG_DBG("KOSync", "Change feed: %s (heap %u)", url.c_str(), (unsigned)ESP.getFreeHeap());

  freeink::SecureHttpClient http;
  if (!beginRequest(http, url)) return NETWORK_ERROR;
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  LOG_DBG("KOSync", "Change feed response: %d", httpCode);
  if (httpCode <= 0) {
    http.end();
    return NETWORK_ERROR;
  }
  if (httpCode == 401) {
    http.end();
    return AUTH_FAILED;
  }
  if (httpCode == 404 || httpCode == 405) {
    http.end();
    return FEED_UNSUPPORTED;
  }
  if (httpCode < 200 || httpCode >= 300) {
    http.end();
    return SERVER_ERROR;
  }

  // Only the fields this reader uses survive the parse; the device list and
  // anything a newer server adds are dropped before they cost heap.
  JsonDocument filter;
  filter["cursor"] = true;
  filter["more"] = true;
  JsonObject changeFilter = filter["changes"].add<JsonObject>();
  changeFilter["document"] = true;
  changeFilter["aliases"] = true;
  changeFilter["percentage"] = true;
  changeFilter["progress"] = true;
  changeFilter["position"] = true;
  changeFilter["device"] = true;
  changeFilter["timestamp"] = true;
  JsonDocument doc;
  DeserializationError error;
  {
    const auto& body = http.getString();
    error = deserializeJson(doc, body.c_str(), DeserializationOption::Filter(filter));
  }
  http.end();
  if (error) {
    LOG_ERR("KOSync", "Change feed JSON parse failed: %s", error.c_str());
    return JSON_ERROR;
  }

  page.cursor = doc["cursor"] | since;
  page.more = doc["more"] | false;
  const JsonArrayConst changes = doc["changes"].as<JsonArrayConst>();
  out.reserve(changes.size());
  for (JsonObjectConst c : changes) {
    KOReaderProgressChange change;
    change.document = c["document"] | "";
    if (change.document.empty()) continue;
    for (JsonVariantConst alias : c["aliases"].as<JsonArrayConst>()) {
      if (change.aliasCount >= KOReaderProgressChange::MAX_ALIASES) break;
      const char* value = alias.as<const char*>();
      if (value && strlen(value) == 32) change.aliases[change.aliasCount++] = value;
    }
    change.percentage = c["percentage"] | 0.0f;
    change.progress = c["progress"] | "";
    change.device = c["device"] | "";
    change.timestamp = c["timestamp"] | static_cast<int64_t>(0);
    const JsonObjectConst pos = c["position"].as<JsonObjectConst>();
    if (!pos.isNull()) {
      KOReaderRichPosition rich;
      rich.pctQ = pos["pctQ"] | 0u;
      rich.spineIndex = pos["spine"] | static_cast<uint16_t>(0);
      rich.pageNumber = pos["page"] | static_cast<uint16_t>(0);
      const uint16_t pages = pos["pages"] | static_cast<uint16_t>(0);
      rich.totalPages = pages > 0 ? pages : 1;
      const uint16_t para = pos["para"] | static_cast<uint16_t>(0);
      if (para > 0) rich.paragraphIndex = para;
      rich.xpath = pos["xpath"] | "";
      change.position = std::move(rich);
    }
    out.push_back(std::move(change));
  }
  page.count = static_cast<uint16_t>(out.size());
  return OK;
}

KOReaderSyncClient::Error KOReaderSyncClient::extensionRequest(const char* endpoint, const char* body, char* reply,
                                                               const size_t replyCapacity) {
  lastHttpCode = 0;
  if (reply && replyCapacity) reply[0] = '\0';
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (!KOREADER_STORE.supportsSyncExtensions()) return EXTENSION_UNSUPPORTED;
  if (!reply || replyCapacity < 2) return JSON_ERROR;
  if (insufficientHeap()) return LOW_MEMORY;

  const std::string url = KOREADER_STORE.getBaseUrl() + endpoint;
  freeink::SecureHttpClient http;
  if (!beginRequest(http, url)) return NETWORK_ERROR;
  // The reply goes into the caller's fixed buffer as it streams in; nothing
  // grows on the heap however large a misbehaving server's answer is.
  size_t length = 0;
  bool overflow = false;
  const auto sink = [&](const uint8_t* data, const size_t n) {
    if (length + n >= replyCapacity) {
      overflow = true;
      return false;
    }
    memcpy(reply + length, data, n);
    length += n;
    return true;
  };
  int httpCode;
  if (body) {
    http.addHeader("Content-Type", "application/json");
    httpCode = http.sendRequest("PUT", reinterpret_cast<const uint8_t*>(body), strlen(body), sink);
  } else {
    httpCode = http.GET(sink);
  }
  http.end();
  lastHttpCode = httpCode;
  reply[length] = '\0';
  LOG_DBG("KOSync", "%s %s -> %d (%u bytes)", body ? "PUT" : "GET", endpoint, httpCode, static_cast<unsigned>(length));

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404 || httpCode == 405) return EXTENSION_UNSUPPORTED;
  if (httpCode <= 0 && !overflow) return NETWORK_ERROR;
  if (overflow) return JSON_ERROR;
  if (httpCode >= 200 && httpCode < 300) return OK;
  return SERVER_ERROR;
}

bool KOReaderProgressChange::matches(const std::string& hash) const {
  if (hash == document) return true;
  for (uint8_t i = 0; i < aliasCount; i++) {
    if (aliases[i] == hash) return true;
  }
  return false;
}

bool KOReaderSyncClient::readerId(char* out, const size_t capacity) {
  if (!out || capacity < READER_ID_SIZE) return false;
  // Computed once: the MAC never changes while running. 27 chars + NUL in
  // .bss, cheaper than re-hashing for every request of a bulk sync.
  static char cached[READER_ID_SIZE] = {};
  if (cached[0] == '\0') {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    // Domain-separating salt: the same MAC also keys the credential
    // obfuscation, so the id must not be the MAC or a plain hash of it.
    MD5Builder md5;
    md5.begin();
    md5.add("KatiePoint sync reader id v1:");
    md5.add(mac, sizeof(mac));
    md5.calculate();
    const String digest = md5.toString();
    snprintf(cached, sizeof(cached), "katiepoint-%.16s", digest.c_str());
  }
  memcpy(out, cached, READER_ID_SIZE);
  return true;
}

void KOReaderSyncClient::deviceIdForServer(char* out, const size_t capacity) {
  if (!out || capacity == 0) return;
  // Strict KOSync servers keep the id every CrossPoint build has always sent:
  // a per-reader id is a CrossPoint Sync extension (one progress row per
  // reader), and KOReader-only servers get nothing they did not get before.
  if (KOREADER_STORE.getServerType() == KOReaderServerType::KOSYNC || !readerId(out, capacity)) {
    snprintf(out, capacity, "%s", DEVICE_ID);
  }
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
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
    case LOW_MEMORY:
      return "Not enough memory for sync, please retry";
    case USER_EXISTS:
      return "Username is already registered";
    case POSITION_UNRESOLVED:
      return "Position could not be mapped for KOReader";
    case FEED_UNSUPPORTED:
      return "Server has no progress change feed";
    case BATCH_UNSUPPORTED:
      return "Server has no batch upload";
    case EXTENSION_UNSUPPORTED:
      return "Requires CrossPoint Sync";
    default:
      return "Unknown error";
  }
}
