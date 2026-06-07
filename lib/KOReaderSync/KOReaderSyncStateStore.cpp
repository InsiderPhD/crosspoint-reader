#include "KOReaderSyncStateStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <cstdio>

void KOReaderSyncStateStore::buildSidecarPath(const char* epubPath, char* outPath, size_t maxLen) {
  MD5Builder md5;
  md5.begin();
  md5.add(epubPath);
  md5.calculate();

  snprintf(outPath, maxLen, "/.crosspoint/koreader_sync_%s.json", md5.toString().c_str());
}

bool KOReaderSyncStateStore::loadLastSyncTimestamp(const char* epubPath, int64_t& outTimestamp) {
  outTimestamp = 0;

  char sidecarPath[72];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));
  if (!Storage.exists(sidecarPath)) {
    return false;
  }

  String json = Storage.readFile(sidecarPath);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    LOG_ERR("KRS", "Sidecar JSON parse error: %s", sidecarPath);
    return false;
  }

  if (doc["last_sync_timestamp"].isNull()) {
    return false;
  }

  outTimestamp = doc["last_sync_timestamp"].as<int64_t>();
  return outTimestamp > 0;
}

bool KOReaderSyncStateStore::saveLastSyncTimestamp(const char* epubPath, const int64_t timestamp) {
  if (timestamp <= 0) {
    return false;
  }

  char sidecarPath[72];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));

  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  if (Storage.exists(sidecarPath)) {
    String existing = Storage.readFile(sidecarPath);
    if (!existing.isEmpty()) {
      deserializeJson(doc, existing);
    }
  }

  doc["last_sync_timestamp"] = timestamp;

  String json;
  serializeJson(doc, json);

  const bool ok = Storage.writeFile(sidecarPath, json);
  if (!ok) {
    LOG_ERR("KRS", "Failed to save KOReader sync timestamp sidecar: %s", sidecarPath);
  }
  return ok;
}

bool KOReaderSyncStateStore::loadLastSyncedPosition(const char* epubPath, KOReaderStoredPosition& out) {
  char sidecarPath[72];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));
  if (!Storage.exists(sidecarPath)) {
    return false;
  }

  String json = Storage.readFile(sidecarPath);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    LOG_ERR("KRS", "Sidecar JSON parse error: %s", sidecarPath);
    return false;
  }

  if (doc["last_sync_spine_index"].isNull()) {
    return false;
  }

  out.percentage = doc["last_sync_percentage"] | 0.0f;
  out.spineIndex = doc["last_sync_spine_index"] | 0;
  out.pageNumber = doc["last_sync_page_number"] | -1;
  out.totalPages = doc["last_sync_total_pages"] | 0;
  return true;
}

bool KOReaderSyncStateStore::saveLastSyncedPosition(const char* epubPath, const KOReaderStoredPosition& position) {
  char sidecarPath[72];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));

  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  if (Storage.exists(sidecarPath)) {
    String existing = Storage.readFile(sidecarPath);
    if (!existing.isEmpty()) {
      deserializeJson(doc, existing);
    }
  }

  doc["last_sync_percentage"] = position.percentage;
  doc["last_sync_spine_index"] = position.spineIndex;
  doc["last_sync_page_number"] = position.pageNumber;
  doc["last_sync_total_pages"] = position.totalPages;

  String json;
  serializeJson(doc, json);

  const bool ok = Storage.writeFile(sidecarPath, json);
  if (!ok) {
    LOG_ERR("KRS", "Failed to save KOReader synced position sidecar: %s", sidecarPath);
  }
  return ok;
}
