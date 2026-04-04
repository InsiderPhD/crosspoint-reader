#include "ReadingStatsStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include "JsonSettingsIO.h"

namespace {
constexpr char STATS_FILE[] = "/.crosspoint/reading_stats.json";
}

ReadingStatsStore ReadingStatsStore::instance;

void ReadingStatsStore::addReadingTime(uint32_t seconds) { totalReadingTimeSeconds += seconds; }

void ReadingStatsStore::addPageTurns(uint32_t count) { totalPagesRead += count; }

void ReadingStatsStore::addBookFinished() { booksFinished++; }

void ReadingStatsStore::addSession() { totalSessions++; }

bool ReadingStatsStore::saveToFile() const {
  JsonDocument doc;
  doc["totalReadingTimeSeconds"] = totalReadingTimeSeconds;
  doc["totalPagesRead"] = totalPagesRead;
  doc["booksFinished"] = booksFinished;
  doc["totalSessions"] = totalSessions;

  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(STATS_FILE, json)) {
    LOG_ERR("RST", "Failed to save reading stats");
    return false;
  }
  LOG_DBG("RST", "Reading stats saved");
  return true;
}

bool ReadingStatsStore::loadFromFile() {
  const String json = Storage.readFile(STATS_FILE);
  if (json.isEmpty()) {
    LOG_DBG("RST", "No reading stats file found, starting fresh");
    return false;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RST", "JSON parse error: %s", error.c_str());
    return false;
  }

  totalReadingTimeSeconds = doc["totalReadingTimeSeconds"] | (uint32_t)0;
  totalPagesRead = doc["totalPagesRead"] | (uint32_t)0;
  booksFinished = doc["booksFinished"] | (uint32_t)0;
  totalSessions = doc["totalSessions"] | (uint32_t)0;

  LOG_DBG("RST", "Reading stats loaded: %lus reading, %lu pages, %lu finished", totalReadingTimeSeconds, totalPagesRead,
          booksFinished);
  return true;
}
