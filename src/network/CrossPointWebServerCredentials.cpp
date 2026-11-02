// Saved OPDS catalogs and WiFi networks over HTTP, in upstream CrossPoint 2.0's
// request/response shapes (docs/webserver-endpoints.md), so companion tools
// written against upstream manage this firmware too. Passwords and header
// values are write-only: a GET only says whether one is set, and an update
// that omits one keeps the stored value.
#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include <array>
#include <string>

#include "CrossPointWebServer.h"
#include "OpdsServerStore.h"
#include "WifiCredentialStore.h"

namespace {
// The body of a JSON POST, parsed into `doc`; false (after answering 400) when
// it is missing or malformed.
bool readJsonBody(WebServer& server, JsonDocument& doc) {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing JSON body");
    return false;
  }
  const DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return false;
  }
  return true;
}

// A string member that is present (even empty), as opposed to absent: absent
// means "keep what is stored" for a secret.
bool hasString(JsonVariantConst value) { return value.is<const char*>(); }

// Streams one serialised object of a JSON array; skips (logs) one that does
// not fit the buffer rather than sending a truncated object.
void sendArrayItem(WebServer& server, const JsonDocument& doc, char* buffer, const size_t size, bool& first) {
  const size_t written = serializeJson(doc, buffer, size);
  if (written >= size) {
    LOG_ERR("WEB", "API item over %u bytes skipped", static_cast<unsigned>(size));
    return;
  }
  if (!first) server.sendContent(",");
  server.sendContent(buffer);
  first = false;
  esp_task_wdt_reset();  // each sendContent() is a blocking network write
}
}  // namespace

// ---- OPDS catalogs ----

// [{"index":0,"name":"...","url":"...","username":"...","hasPassword":true,
//   "headers":[{"name":"...","hasValue":true},...]}]
void CrossPointWebServer::handleGetOpdsServers() const {
  const auto& servers = OPDS_STORE.getServers();
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  // One server at a time through a fixed buffer, never the whole array.
  char output[768];
  JsonDocument doc;
  bool first = true;
  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    doc["hasPassword"] = !servers[i].password.empty();
    JsonArray headers = doc["headers"].to<JsonArray>();
    for (const auto& header : servers[i].customHeaders) {
      JsonObject h = headers.add<JsonObject>();
      h["name"] = header.name;
      h["hasValue"] = !header.value.empty();
    }
    sendArrayItem(*server, doc, output, sizeof(output), first);
  }
  server->sendContent("]");
  server->sendContent("");
}

// {"index"?:n, "name","url","username", "password"?, "headers"?:[{"name","value"?},...]}
// With "index" it updates that catalog: an absent password or header value
// keeps the stored one; an empty header name clears that slot.
void CrossPointWebServer::handlePostOpdsServer() {
  JsonDocument doc;
  if (!readJsonBody(*server, doc)) return;

  OpdsServer entry;
  entry.name = doc["name"] | std::string("");
  entry.url = doc["url"] | std::string("");
  entry.username = doc["username"] | std::string("");
  const bool hasPassword = hasString(doc["password"]);
  const std::string password = doc["password"] | std::string("");

  const bool hasHeaders = doc["headers"].is<JsonArrayConst>();
  std::array<std::string, OpdsServer::MAX_CUSTOM_HEADERS> headerNames;
  std::array<std::string, OpdsServer::MAX_CUSTOM_HEADERS> headerValues;
  std::array<bool, OpdsServer::MAX_CUSTOM_HEADERS> headerValueGiven{};
  if (hasHeaders) {
    size_t slot = 0;
    for (JsonObjectConst h : doc["headers"].as<JsonArrayConst>()) {
      if (slot >= OpdsServer::MAX_CUSTOM_HEADERS) break;
      headerNames[slot] = h["name"] | std::string("");
      headerValueGiven[slot] = hasString(h["value"]);
      headerValues[slot] = h["value"] | std::string("");
      slot++;
    }
  }

  if (doc["index"].is<int>()) {
    const int index = doc["index"].as<int>();
    const OpdsServer* existing = index >= 0 ? OPDS_STORE.getServer(static_cast<size_t>(index)) : nullptr;
    if (!existing) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    entry.password = hasPassword ? password : existing->password;
    entry.filenameFormat = existing->filenameFormat;
    for (size_t i = 0; i < OpdsServer::MAX_CUSTOM_HEADERS; i++) {
      if (!hasHeaders) {
        entry.customHeaders[i] = existing->customHeaders[i];
      } else if (!headerNames[i].empty()) {
        entry.customHeaders[i].name = headerNames[i];
        entry.customHeaders[i].value = headerValueGiven[i] ? headerValues[i] : existing->customHeaders[i].value;
      }
      // An empty name leaves the slot empty: never a stale value without a name.
    }
    if (!OPDS_STORE.updateServer(static_cast<size_t>(index), entry)) {
      server->send(500, "text/plain", "Failed to save server");
      return;
    }
    LOG_DBG("WEB", "Updated OPDS server %d", index);
  } else {
    entry.password = password;
    for (size_t i = 0; i < OpdsServer::MAX_CUSTOM_HEADERS; i++) {
      if (headerNames[i].empty()) continue;
      entry.customHeaders[i].name = headerNames[i];
      entry.customHeaders[i].value = headerValues[i];
    }
    if (!OPDS_STORE.addServer(entry)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added OPDS server: %s", entry.name.c_str());
  }
  server->send(200, "text/plain", "OK");
}

// {"index":n}. POST rather than DELETE: the ESP32 WebServer takes no DELETE body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  JsonDocument doc;
  if (!readJsonBody(*server, doc)) return;
  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }
  const int index = doc["index"].as<int>();
  if (index < 0 || static_cast<size_t>(index) >= OPDS_STORE.getCount()) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }
  if (!OPDS_STORE.removeServer(static_cast<size_t>(index))) {
    server->send(500, "text/plain", "Failed to save");
    return;
  }
  LOG_DBG("WEB", "Deleted OPDS server %d", index);
  server->send(200, "text/plain", "OK");
}

// ---- WiFi networks ----

namespace {
// WIFI_STORE is loaded by whoever needs it (WiFi selection, sync), not at boot:
// in hotspot mode nothing has. Re-read it first so a POST never saves a list
// that is missing the networks on the card.
void reloadWifiStore() { WIFI_STORE.loadFromFile(); }
}  // namespace

// [{"index":0,"ssid":"...","hasPassword":true,"isLastConnected":true}]
void CrossPointWebServer::handleGetWifiNetworks() const {
  reloadWifiStore();
  const auto& credentials = WIFI_STORE.getCredentials();
  const std::string& last = WIFI_STORE.getLastConnectedSsid();
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[256];
  JsonDocument doc;
  bool first = true;
  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    doc["hasPassword"] = !credentials[i].password.empty();
    doc["isLastConnected"] = !last.empty() && credentials[i].ssid == last;
    sendArrayItem(*server, doc, output, sizeof(output), first);
  }
  server->sendContent("]");
  server->sendContent("");
}

// {"index"?:n, "ssid", "password"?}. An absent password keeps the stored one on
// an update; an empty one is an open network.
void CrossPointWebServer::handlePostWifiNetwork() {
  reloadWifiStore();
  JsonDocument doc;
  if (!readJsonBody(*server, doc)) return;

  const std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }
  const bool hasPassword = hasString(doc["password"]);
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    const int index = doc["index"].as<int>();
    const auto& credentials = WIFI_STORE.getCredentials();
    if (index < 0 || static_cast<size_t>(index) >= credentials.size()) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }
    // Copies: removeCredential() below invalidates the reference.
    const std::string oldSsid = credentials[index].ssid;
    if (!hasPassword) password = credentials[index].password;
    const bool ok = oldSsid == ssid ? WIFI_STORE.addCredential(ssid, password)
                                    : WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }
    LOG_DBG("WEB", "Updated WiFi network %d (%s)", index, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added WiFi network: %s", ssid.c_str());
  }
  server->send(200, "text/plain", "OK");
}

// {"index":n}
void CrossPointWebServer::handleDeleteWifiNetwork() {
  reloadWifiStore();
  JsonDocument doc;
  if (!readJsonBody(*server, doc)) return;
  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }
  const int index = doc["index"].as<int>();
  const auto& credentials = WIFI_STORE.getCredentials();
  if (index < 0 || static_cast<size_t>(index) >= credentials.size()) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }
  const std::string ssid = credentials[index].ssid;
  if (!WIFI_STORE.removeCredential(ssid)) {
    server->send(500, "text/plain", "Failed to delete network");
    return;
  }
  LOG_DBG("WEB", "Deleted WiFi network %d (%s)", index, ssid.c_str());
  server->send(200, "text/plain", "OK");
}
