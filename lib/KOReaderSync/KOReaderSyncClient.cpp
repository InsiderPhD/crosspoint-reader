#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <base64.h>

#include <cstdlib>
#include <ctime>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
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

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  // Build JSON body
  JsonDocument doc;
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
      if (!m.series.empty()) meta["series"] = m.series;
      if (m.seriesIndex.has_value()) meta["series_index"] = *m.seriesIndex;
    }
  }
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

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
    default:
      return "Unknown error";
  }
}
