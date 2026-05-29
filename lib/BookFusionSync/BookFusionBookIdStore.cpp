#include "BookFusionBookIdStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <cstdio>
#include <cstring>

void BookFusionBookIdStore::buildSidecarPath(const char* epubPath, char* outPath, size_t maxLen) {
  MD5Builder md5;
  md5.begin();
  md5.add(epubPath);
  md5.calculate();

  // Result: /.crosspoint/bookfusion_<32hexchars>.json  (55 chars total)
  snprintf(outPath, maxLen, "/.crosspoint/bookfusion_%s.json", md5.toString().c_str());
}

uint32_t BookFusionBookIdStore::loadBookId(const char* epubPath) {
  char sidecarPath[64];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));

  if (!Storage.exists(sidecarPath)) {
    return 0;
  }

  String json = Storage.readFile(sidecarPath);
  if (json.isEmpty()) {
    return 0;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    LOG_ERR("BFS", "Sidecar JSON parse error: %s", sidecarPath);
    return 0;
  }

  const uint32_t bookId = doc["book_id"] | (uint32_t)0;
  LOG_DBG("BFS", "Loaded book_id=%lu for %s", (unsigned long)bookId, epubPath);
  return bookId;
}

bool BookFusionBookIdStore::loadLastSyncAt(const char* epubPath, char* out, size_t maxLen) {
  if (out == nullptr || maxLen == 0) {
    return false;
  }
  out[0] = '\0';

  char sidecarPath[64];
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
    LOG_ERR("BFS", "Sidecar JSON parse error: %s", sidecarPath);
    return false;
  }

  const char* lastSyncAt = doc["last_sync_at"] | "";
  if (lastSyncAt[0] == '\0') {
    return false;
  }

  strlcpy(out, lastSyncAt, maxLen);
  return true;
}

bool BookFusionBookIdStore::loadLastSyncedPosition(const char* epubPath, BookFusionStoredPosition& out) {
  char sidecarPath[64];
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
    LOG_ERR("BFS", "Sidecar JSON parse error: %s", sidecarPath);
    return false;
  }

  if (doc["last_sync_chapter_index"].isNull() || doc["last_sync_page_position_in_book"].isNull()) {
    return false;
  }

  out.percentage = doc["last_sync_percentage"] | 0.0f;
  out.chapterIndex = doc["last_sync_chapter_index"] | 0;
  out.pagePositionInBook = doc["last_sync_page_position_in_book"] | 0.0f;
  out.pageNumber = doc["last_sync_page_number"] | -1;
  out.totalPages = doc["last_sync_total_pages"] | 0;
  return true;
}

bool BookFusionBookIdStore::hasBookId(const char* epubPath) {
  char sidecarPath[64];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));
  return Storage.exists(sidecarPath);
}

bool BookFusionBookIdStore::saveBookId(const char* epubPath, uint32_t bookId) {
  if (bookId == 0) {
    LOG_ERR("BFS", "Refusing to save book_id=0 for %s", epubPath);
    return false;
  }

  char sidecarPath[64];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));

  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  if (Storage.exists(sidecarPath)) {
    String existing = Storage.readFile(sidecarPath);
    if (!existing.isEmpty()) {
      deserializeJson(doc, existing);
    }
  }
  doc["book_id"] = bookId;

  String json;
  serializeJson(doc, json);

  const bool ok = Storage.writeFile(sidecarPath, json);
  if (ok) {
    LOG_DBG("BFS", "Saved book_id=%lu for %s", (unsigned long)bookId, epubPath);
  } else {
    LOG_ERR("BFS", "Failed to save sidecar: %s", sidecarPath);
  }
  return ok;
}

bool BookFusionBookIdStore::saveLastSyncAt(const char* epubPath, const char* updatedAt) {
  if (updatedAt == nullptr || updatedAt[0] == '\0') {
    return false;
  }

  char sidecarPath[64];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));

  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  if (Storage.exists(sidecarPath)) {
    String existing = Storage.readFile(sidecarPath);
    if (!existing.isEmpty()) {
      deserializeJson(doc, existing);
    }
  }
  doc["last_sync_at"] = updatedAt;

  String json;
  serializeJson(doc, json);

  const bool ok = Storage.writeFile(sidecarPath, json);
  if (ok) {
    LOG_DBG("BFS", "Saved last_sync_at=%s for %s", updatedAt, epubPath);
  } else {
    LOG_ERR("BFS", "Failed to save last_sync_at sidecar: %s", sidecarPath);
  }
  return ok;
}

bool BookFusionBookIdStore::saveLastSyncedPosition(const char* epubPath, const BookFusionStoredPosition& position) {
  char sidecarPath[64];
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
  doc["last_sync_chapter_index"] = position.chapterIndex;
  doc["last_sync_page_position_in_book"] = position.pagePositionInBook;
  doc["last_sync_page_number"] = position.pageNumber;
  doc["last_sync_total_pages"] = position.totalPages;

  String json;
  serializeJson(doc, json);

  const bool ok = Storage.writeFile(sidecarPath, json);
  if (ok) {
    LOG_DBG("BFS", "Saved last synced position: %.2f%%, chapter %d", position.percentage, position.chapterIndex);
  } else {
    LOG_ERR("BFS", "Failed to save last synced position sidecar: %s", sidecarPath);
  }
  return ok;
}
