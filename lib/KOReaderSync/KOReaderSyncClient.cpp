#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include <ctime>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// Transport is freeink::SecureHttpClient (wolfSSL). This matters beyond RAM:
// the official KOSync server (kosync.ak-team.com:3042) is TLS 1.3-only, and
// the precompiled system mbedTLS ships TLS 1.3 as empty stubs — this port is
// what makes it reachable at all. setInsecure() matches the BookFusion client
// and the previous esp-tls posture (CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY):
// self-hosted kosync / Calibre-Web servers commonly run https with
// self-signed certificates, so server verification must stay off.
//
// Connection-per-request (setReuse(false)), like the esp_http_client it
// replaces: the sync flows make one or two calls, and the client object lives
// on the stack of each method.
//
// Returns false on a malformed URL. NOTE: begin() clears headers, so they are
// added after it.
bool beginRequest(freeink::SecureHttpClient& http, const std::string& url) {
  http.setInsecure();
  http.setTimeout(15000);
  http.setReuse(false);
  // HTTP Basic Auth for Calibre-Web-Automated compatibility
  http.setBasicAuth(KOREADER_STORE.getUsername(), KOREADER_STORE.getPassword());
  if (!http.begin(url)) return false;
  // KOSync auth headers
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  return true;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());

  freeink::SecureHttpClient http;
  if (!beginRequest(http, url)) return NETWORK_ERROR;

  const int httpCode = http.GET();
  lastHttpCode = httpCode < 0 ? 0 : httpCode;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode < 0) return NETWORK_ERROR;
  if (httpCode == 200) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());

  freeink::SecureHttpClient http;
  if (!beginRequest(http, url)) return NETWORK_ERROR;

  const int httpCode = http.GET();
  lastHttpCode = httpCode < 0 ? 0 : httpCode;

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode < 0) return NETWORK_ERROR;

  if (httpCode == 200 && !http.getString().empty()) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, http.getString());

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

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
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
  lastHttpCode = httpCode < 0 ? 0 : httpCode;

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode < 0) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
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
    default:
      return "Unknown error";
  }
}
