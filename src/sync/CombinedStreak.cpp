#include "CombinedStreak.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>

namespace CombinedStreak {
namespace {
constexpr char PATH[] = "/.crosspoint/kosync_streak.json";
Value cache;
bool loaded = false;
}  // namespace

const Value& get() {
  if (loaded) return cache;
  loaded = true;
  std::string json;
  if (!Storage.exists(PATH) || !Storage.readFileToString("STREAK", PATH, 128, json)) return cache;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return cache;
  cache.current = doc["current"] | 0u;
  cache.longest = doc["longest"] | 0u;
  cache.anchorOrdinal = doc["anchor"] | 0u;
  cache.valid = cache.anchorOrdinal != 0;
  return cache;
}

uint32_t currentAsOf(const uint32_t todayOrdinal) {
  const Value& v = get();
  if (!v.valid) return 0;
  // Unknown date: show what the server said.
  if (todayOrdinal == 0) return v.current;
  return v.anchorOrdinal + 1 >= todayOrdinal ? v.current : 0;
}

bool save(const uint32_t current, const uint32_t longest, const uint32_t anchorOrdinal) {
  const Value& old = get();
  if (old.valid && old.current == current && old.longest == longest && old.anchorOrdinal == anchorOrdinal) return true;
  JsonDocument doc;
  doc["current"] = current;
  doc["longest"] = longest;
  doc["anchor"] = anchorOrdinal;
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(PATH, json)) {
    LOG_ERR("STREAK", "Could not save the combined streak");
    return false;
  }
  cache.valid = anchorOrdinal != 0;
  cache.current = current;
  cache.longest = longest;
  cache.anchorOrdinal = anchorOrdinal;
  return true;
}

}  // namespace CombinedStreak
