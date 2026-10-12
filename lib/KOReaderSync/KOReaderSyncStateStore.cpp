#include "KOReaderSyncStateStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <cstdio>

#include "KOReaderCredentialStore.h"

namespace {
// Sync state is only meaningful against the server it was exchanged with, so
// the sidecar records the profile slot it belongs to. A sidecar written before
// profiles existed has no key and belongs to profile 0, which is where the
// single pre-profile account was migrated.
constexpr char PROFILE_KEY[] = "profile";

bool belongsToActiveProfile(const JsonDocument& doc) {
  const uint8_t stored = doc[PROFILE_KEY] | static_cast<uint8_t>(0);
  return stored == KOREADER_STORE.getActiveProfile();
}

// Called before a save merges new fields into an existing sidecar: state left
// by another profile must not survive next to the new profile's fields.
void claimForActiveProfile(JsonDocument& doc) {
  if (!belongsToActiveProfile(doc)) {
    doc.remove("last_sync_timestamp");
    doc.remove("last_sync_percentage");
    doc.remove("last_sync_spine_index");
    doc.remove("last_sync_page_number");
    doc.remove("last_sync_total_pages");
  }
  doc[PROFILE_KEY] = KOREADER_STORE.getActiveProfile();
}
}  // namespace

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

  if (!belongsToActiveProfile(doc) || doc["last_sync_timestamp"].isNull()) {
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

  claimForActiveProfile(doc);
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

  if (!belongsToActiveProfile(doc) || doc["last_sync_spine_index"].isNull()) {
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

  claimForActiveProfile(doc);
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

bool KOReaderSyncStateStore::movePath(const char* oldEpubPath, const char* newEpubPath) {
  char oldSidecar[72];
  char newSidecar[72];
  buildSidecarPath(oldEpubPath, oldSidecar, sizeof(oldSidecar));
  buildSidecarPath(newEpubPath, newSidecar, sizeof(newSidecar));
  if (!Storage.exists(oldSidecar)) return true;
  // Anything already at the new key belongs to a book that is gone (the caller
  // checked the new file name was free), so it is replaced.
  if (Storage.exists(newSidecar)) Storage.remove(newSidecar);
  if (!Storage.rename(oldSidecar, newSidecar)) {
    LOG_ERR("KRS", "Failed to move sidecar %s -> %s", oldSidecar, newSidecar);
    return false;
  }
  return true;
}
