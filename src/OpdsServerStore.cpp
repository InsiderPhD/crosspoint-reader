#include "OpdsServerStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "util/OpdsDownloadFolder.h"

OpdsServerStore OpdsServerStore::instance;

namespace {
constexpr char OPDS_FILE_JSON[] = "/.crosspoint/opds.json";
// Name given to the server migrated out of settings (data, not UI text: it is
// what upstream and CrossInk name the same migration).
constexpr char MIGRATED_SERVER_NAME[] = "OPDS Server";

// Obfuscated secret, or the plaintext key a hand-written file may carry
// instead. Sets resave when a plaintext secret was found.
std::string readSecret(JsonObjectConst obj, const char* obfKey, const char* plainKey, bool& resave) {
  bool ok = false;
  std::string value = obfuscation::deobfuscateFromBase64(obj[obfKey] | "", &ok);
  if (ok && !value.empty()) return value;
  value = obj[plainKey] | std::string("");
  if (!value.empty()) resave = true;
  return value;
}
}  // namespace

void OpdsServerStore::ensureLoaded() const {
  if (loaded) return;
  // Lazy first load from a const accessor; the store is a process-wide
  // singleton touched only from the main loop task.
  const_cast<OpdsServerStore*>(this)->loadFromFile();
}

bool OpdsServerStore::loadFromFile() {
  loaded = true;
  servers.clear();

  if (!Storage.exists(OPDS_FILE_JSON)) {
    return migrateFromSettings();
  }

  const String json = Storage.readFile(OPDS_FILE_JSON);
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, json.c_str());
  if (error) {
    // Leave the file alone: a later save would otherwise wipe catalogs another
    // firmware wrote in a form we failed to read.
    LOG_ERR("OPS", "opds.json parse error: %s", error.c_str());
    return false;
  }

  // A missing or non-array "servers" is an empty list (a null array iterates
  // zero times), as upstream.
  const JsonArrayConst arr = doc["servers"].as<JsonArrayConst>();
  servers.reserve(std::min(arr.size(), MAX_SERVERS));
  bool resave = false;
  for (JsonObjectConst obj : arr) {
    if (servers.size() >= MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | std::string("");
    server.url = obj["url"] | std::string("");
    server.username = obj["username"] | std::string("");
    server.password = readSecret(obj, "password_obf", "password", resave);
    server.filenameFormat = obj["filenameFormat"] | std::string("");
    // An unusable folder from a hand edit falls back to the shared default
    // rather than dropping the catalog.
    if (!OpdsDownloadFolder::normalize(obj["downloadFolder"] | "", server.downloadFolder)) {
      LOG_ERR("OPS", "Ignoring invalid download folder for %s", server.name.c_str());
      server.downloadFolder.clear();
    }
    size_t slot = 0;
    for (JsonObjectConst headerObj : obj["headers"].as<JsonArrayConst>()) {
      if (slot >= OpdsServer::MAX_CUSTOM_HEADERS) break;
      server.customHeaders[slot].name = headerObj["name"] | std::string("");
      server.customHeaders[slot].value = readSecret(headerObj, "value_obf", "value", resave);
      slot++;
    }
    servers.push_back(std::move(server));
  }
  LOG_DBG("OPS", "Loaded %u OPDS server(s)", static_cast<unsigned>(servers.size()));

  if (resave) {
    LOG_DBG("OPS", "Resaving opds.json with obfuscated secrets");
    saveToFile();
  }
  return true;
}

bool OpdsServerStore::saveToFile() const {
  JsonDocument doc;
  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : servers) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
    // Every slot, empty ones included, so a slot keeps its position (upstream
    // reads them positionally).
    JsonArray headers = obj["headers"].to<JsonArray>();
    for (const auto& header : server.customHeaders) {
      JsonObject headerObj = headers.add<JsonObject>();
      headerObj["name"] = header.name;
      headerObj["value_obf"] = obfuscation::obfuscateToBase64(header.value);
    }
    if (!server.filenameFormat.empty()) obj["filenameFormat"] = server.filenameFormat;
    obj["downloadFolder"] = server.downloadFolder;
  }

  String json;
  serializeJson(doc, json);
  Storage.mkdir("/.crosspoint");
  if (!Storage.writeFile(OPDS_FILE_JSON, json)) {
    LOG_ERR("OPS", "Failed to write opds.json");
    return false;
  }
  return true;
}

// The single server this firmware kept in settings becomes the first catalog.
// Runs only while opds.json does not exist; once written, an empty list stays
// empty. The legacy fields are cleared afterwards so the password does not
// linger in settings.json alongside its new home.
bool OpdsServerStore::migrateFromSettings() {
  if (SETTINGS.opdsServerUrl[0] == '\0') return false;

  OpdsServer server;
  server.name = MIGRATED_SERVER_NAME;
  server.url = SETTINGS.opdsServerUrl;
  server.username = SETTINGS.opdsUsername;
  server.password = SETTINGS.opdsPassword;
  servers.push_back(std::move(server));

  if (!saveToFile()) {
    // Keep the in-memory copy so this session can still browse; the next boot
    // retries the migration.
    return true;
  }
  SETTINGS.opdsServerUrl[0] = '\0';
  SETTINGS.opdsUsername[0] = '\0';
  SETTINGS.opdsPassword[0] = '\0';
  SETTINGS.saveToFile();
  LOG_INF("OPS", "Migrated the OPDS server from settings to opds.json");
  return true;
}

bool OpdsServerStore::addServer(const OpdsServer& server) {
  ensureLoaded();
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %u reached", static_cast<unsigned>(MAX_SERVERS));
    return false;
  }
  servers.push_back(server);
  if (saveToFile()) return true;
  servers.pop_back();
  return false;
}

bool OpdsServerStore::updateServer(const size_t index, const OpdsServer& server) {
  ensureLoaded();
  if (index >= servers.size()) return false;
  OpdsServer previous = std::move(servers[index]);
  servers[index] = server;
  if (saveToFile()) return true;
  servers[index] = std::move(previous);
  return false;
}

bool OpdsServerStore::removeServer(const size_t index) {
  ensureLoaded();
  if (index >= servers.size()) return false;
  servers.erase(servers.begin() + static_cast<std::ptrdiff_t>(index));
  return saveToFile();
}

const OpdsServer* OpdsServerStore::getServer(const size_t index) const {
  ensureLoaded();
  return index < servers.size() ? &servers[index] : nullptr;
}

std::vector<HttpDownloader::Header> OpdsServer::activeCustomHeaders() const {
  std::vector<HttpDownloader::Header> active;
  active.reserve(MAX_CUSTOM_HEADERS);
  for (const auto& header : customHeaders) {
    if (!header.name.empty()) active.emplace_back(header.name, header.value);
  }
  return active;
}
