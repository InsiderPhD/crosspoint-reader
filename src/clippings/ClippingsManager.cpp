#include "ClippingsManager.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "util/TimeUtils.h"

namespace {
// Kindle-style "Added on ..." line built from the NTP/manually-set system clock (see
// util/WifiTimeSync.cpp / util/TimeUtils.h). Returns false when the clock has never been set,
// in which case the caller omits the timestamp. Uses the biased quarter-hour UTC offset from
// settings (48 = UTC+0) and gmtime_r so it does not depend on TZ configuration.
bool formatKindleAddedOn(char* buf, const size_t bufSize) {
  if (!buf || bufSize == 0) return false;

  const uint32_t epoch = TimeUtils::getCurrentValidTimestamp();
  if (!TimeUtils::isClockValid(epoch)) return false;

  const uint8_t offsetQ = std::min<uint8_t>(SETTINGS.clockUtcOffsetQ, 104);
  const long offsetSeconds = (static_cast<long>(offsetQ) - 48) * 15L * 60L;

  const time_t localEpoch = static_cast<time_t>(epoch) + offsetSeconds;
  struct tm tmLocal;
  if (gmtime_r(&localEpoch, &tmLocal) == nullptr) return false;

  // e.g. "Added on Wednesday, July 8, 2026, 3:04 PM" (%e is space-padded; collapse the gap).
  char tmp[80];
  if (strftime(tmp, sizeof(tmp), "Added on %A, %B %e, %Y, %I:%M %p", &tmLocal) == 0) return false;

  // Collapse the double space %e leaves for single-digit days and strip a leading zero on the hour.
  std::string out = tmp;
  const size_t dbl = out.find("  ");
  if (dbl != std::string::npos) out.erase(dbl, 1);
  snprintf(buf, bufSize, "%s", out.c_str());
  return true;
}
}  // namespace

bool ClippingsManager::saveClipping(const std::string& bookTitle, const std::string& author,
                                    const std::string& chapterTitle, const int pageNumber,
                                    const std::string& selectedText) {
  FsFile file = Storage.open(CLIPPINGS_PATH, O_WRITE | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("CLIP", "Failed to open %s for append", CLIPPINGS_PATH);
    return false;
  }

  std::string location = "- Your Highlight on Page " + std::to_string(pageNumber);
  if (!chapterTitle.empty()) {
    location += " | " + chapterTitle;
  }
  char addedOn[96];
  if (formatKindleAddedOn(addedOn, sizeof(addedOn))) {
    location += " | ";
    location += addedOn;
  }
  location += "\n";

  static constexpr size_t MAX_TEXT_BYTES = 2000;
  const size_t textLen = std::min(selectedText.size(), MAX_TEXT_BYTES);
  static constexpr char separator[] = "\n==========\n";

  std::string buffer;
  buffer.reserve(bookTitle.size() + author.size() + location.size() + textLen + sizeof(separator) + 8);
  buffer += bookTitle;
  buffer += " (";
  buffer += author;
  buffer += ")\n";
  buffer += location;
  buffer += '\n';
  buffer.append(selectedText.c_str(), textLen);
  buffer += separator;

  const bool ok = file.write(buffer.data(), buffer.size()) == buffer.size();
  file.flush();
  file.close();

  if (!ok) {
    LOG_ERR("CLIP", "Failed to write clipping to %s", CLIPPINGS_PATH);
    return false;
  }

  LOG_DBG("CLIP", "Saved clipping to %s (%zu bytes)", CLIPPINGS_PATH, textLen);
  return true;
}
