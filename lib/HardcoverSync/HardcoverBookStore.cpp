#include "HardcoverBookStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <cstdio>
#include <cstring>

void HardcoverBookStore::buildSidecarPath(const char* epubPath, char* outPath, const size_t maxLen) {
  MD5Builder md5;
  md5.begin();
  md5.add(epubPath);
  md5.calculate();
  snprintf(outPath, maxLen, "/.crosspoint/hardcover_%s.json", md5.toString().c_str());
}

bool HardcoverBookStore::load(const char* epubPath, HardcoverBookLink& out) {
  out = HardcoverBookLink{};

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
    LOG_ERR("HCB", "Sidecar JSON parse error: %s", sidecarPath);
    return false;
  }

  out.bookId = doc["book_id"] | 0;
  out.editionId = doc["edition_id"] | 0;
  out.pages = doc["pages"] | 0;
  out.userBookId = doc["user_book_id"] | 0;
  out.readId = doc["read_id"] | 0;
  strlcpy(out.startedAt, doc["started_at"] | "", sizeof(out.startedAt));
  out.syncedPages = doc["synced_pages"] | -1;
  out.syncedPercent = doc["synced_percent"] | 0.0f;
  out.syncedThroughDay = doc["synced_day"] | 0u;
  out.finished = doc["finished"] | false;
  out.lastMissEpoch = doc["miss_at"] | 0u;
  return true;
}

bool HardcoverBookStore::save(const char* epubPath, const HardcoverBookLink& link) {
  char sidecarPath[72];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["book_id"] = link.bookId;
  doc["edition_id"] = link.editionId;
  doc["pages"] = link.pages;
  doc["user_book_id"] = link.userBookId;
  doc["read_id"] = link.readId;
  doc["started_at"] = link.startedAt;
  doc["synced_pages"] = link.syncedPages;
  doc["synced_percent"] = link.syncedPercent;
  doc["synced_day"] = link.syncedThroughDay;
  doc["finished"] = link.finished;
  doc["miss_at"] = link.lastMissEpoch;

  String json;
  serializeJson(doc, json);
  const bool ok = Storage.writeFile(sidecarPath, json);
  if (!ok) {
    LOG_ERR("HCB", "Failed to save sidecar: %s", sidecarPath);
  }
  return ok;
}
