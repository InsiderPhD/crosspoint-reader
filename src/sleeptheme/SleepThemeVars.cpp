#include "SleepThemeVars.h"

#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SleepThemeExpr.h"
#include "SleepThemePluginVars.h"
#include "util/LibraryReadingStats.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"
#include "util/TimeZoneRegistry.h"

namespace SleepThemeVars {

namespace {

constexpr Info INFOS[] = {
#define SLEEP_THEME_VAR_INFO(id, name, kind, scan) {name, Kind::kind, scan},
    SLEEP_THEME_VARS(SLEEP_THEME_VAR_INFO)
#undef SLEEP_THEME_VAR_INFO
};
static_assert(sizeof(INFOS) / sizeof(INFOS[0]) == static_cast<size_t>(Id::Count), "var table out of sync");

constexpr uint64_t MS_PER_MINUTE = 60ULL * 1000ULL;

// Stat computations the old sleep-screen stats card owned. Each returns false
// when the value cannot be known (no book open, clock not set, ...).

// Day of week for a Gregorian date via Sakamoto's algorithm. 0 = Sunday .. 6 = Saturday.
int dayOfWeek(int y, unsigned m, unsigned d) {
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + static_cast<int>(d)) % 7;
}

bool estimateBookTimeLeftMs(uint64_t& outMs) {
  if (APP_STATE.openEpubPath.empty()) return false;
  // Prefer the reader's own pages-left × pace figure, stashed at reader
  // exit — the same estimate the status bar / reader menu show, and it
  // doesn't hide after short sessions the way percent-based pace does.
  if (APP_STATE.readerTimeLeftSeconds > 0 && APP_STATE.readerTimeLeftBookPath == APP_STATE.openEpubPath) {
    outMs = static_cast<uint64_t>(APP_STATE.readerTimeLeftSeconds) * 1000ULL;
    return true;
  }
  // Fallback: "read (end-start)% of the book in sessionMs, so (100-end)%
  // remaining takes remaining/pace". Recent-reading only, so it survives a
  // stats reset, unlike extrapolating lifetime time vs. progress.
  const ReadingSessionSnapshot& snap = READING_STATS.getLastSessionSnapshot();
  if (snap.valid && snap.path == APP_STATE.openEpubPath && snap.sessionMs != 0 &&
      snap.endProgressPercent > snap.startProgressPercent && snap.endProgressPercent < 100) {
    const uint32_t gainedPct = snap.endProgressPercent - snap.startProgressPercent;
    const uint32_t remainingPct = 100 - snap.endProgressPercent;
    outMs = static_cast<uint64_t>(snap.sessionMs) * remainingPct / gainedPct;
    return true;
  }
  return false;
}

bool currentWeekGoalMask(uint8_t& outMask) {
  // Future days this week simply stay unset.
  const uint32_t todayOrd = TimeUtils::getLocalDayOrdinal(READING_STATS.getDisplayTimestamp());
  int ty = 0;
  unsigned tm = 0, tdd = 0;
  if (!TimeUtils::getDateFromDayOrdinal(todayOrd, ty, tm, tdd)) return false;  // clock not set
  const int dow = dayOfWeek(ty, tm, tdd);                                      // 0 = Sunday
  const uint32_t mondayOrd = todayOrd - static_cast<uint32_t>((dow + 6) % 7);
  const uint64_t goalMs = SETTINGS.getDailyGoalMs();
  uint8_t mask = 0;
  for (uint8_t d = 0; d < 7; d++) {  // Monday..Sunday
    const uint32_t ord = mondayOrd + d;
    uint64_t ms = 0;
    for (const auto& rd : READING_STATS.getReadingDays()) {
      if (rd.dayOrdinal == ord) {
        ms = rd.readingMs;
        break;
      }
    }
    if (ms >= goalMs) mask = static_cast<uint8_t>(mask | (1u << d));
  }
  outMask = mask;
  return true;
}

bool daysReadThisMonth(uint32_t& outDays) {
  // Count reading days whose (year, month) matches the reference date —
  // mirrors buildMonthSummary() in ReadingStatsActivity.
  const uint32_t refOrdinal = TimeUtils::getLocalDayOrdinal(READING_STATS.getDisplayTimestamp());
  int refYear = 0;
  unsigned refMonth = 0, refDay = 0;
  if (!TimeUtils::getDateFromDayOrdinal(refOrdinal, refYear, refMonth, refDay)) return false;  // clock not set
  uint32_t daysRead = 0;
  for (const auto& d : READING_STATS.getReadingDays()) {
    if (d.readingMs == 0) continue;
    int y = 0;
    unsigned m = 0, dd = 0;
    if (TimeUtils::getDateFromDayOrdinal(d.dayOrdinal, y, m, dd) && y == refYear && m == refMonth) {
      daysRead++;
    }
  }
  outDays = daysRead;
  return true;
}

// Per-frame caches. Heap, between beginFrame() and endFrame() only: the
// Library summary alone is ~600 bytes, too much for the stack and not worth
// holding in static DRAM for a screen drawn once per sleep.
struct FrameCache {
  bool bookResolved = false;
  const ReadingBookStats* book = nullptr;
  bool libraryDone = false;
  LibraryReadingStats::Summary library;
  bool profileDone = false;
  ReadingStatsAnalytics::ReadingProfileSummary profile;
  bool clippingsDone = false;
  uint16_t clippings = 0;
  bool recentsDone = false;
  uint8_t recentCount = 0;
  uint8_t recentIdx[RECENT_SLOTS] = {};  // indices into RECENT_BOOKS.getBooks()
};
std::unique_ptr<FrameCache> cache;

const ReadingBookStats* currentBook() {
  if (!cache) {
    return APP_STATE.openEpubPath.empty() ? nullptr : READING_STATS.findMatchingBookForPath(APP_STATE.openEpubPath);
  }
  if (!cache->bookResolved) {
    cache->bookResolved = true;
    cache->book =
        APP_STATE.openEpubPath.empty() ? nullptr : READING_STATS.findMatchingBookForPath(APP_STATE.openEpubPath);
  }
  return cache->book;
}

// Null when no frame is open (or its allocation failed): the value is unknown.
const LibraryReadingStats::Summary* library() {
  if (!cache) return nullptr;
  if (!cache->libraryDone) {
    cache->libraryDone = true;
    LibraryReadingStats::compute(cache->library);
  }
  return &cache->library;
}

// Null when no frame is open or there is no reading to profile yet.
const ReadingStatsAnalytics::ReadingProfileSummary* profile() {
  if (!cache) return nullptr;
  if (!cache->profileDone) {
    cache->profileDone = true;
    cache->profile = ReadingStatsAnalytics::buildReadingProfileSummary();
  }
  return cache->profile.hasData ? &cache->profile : nullptr;
}

int64_t percentOf(const uint64_t part, const uint64_t whole) {
  return whole == 0 ? 0 : static_cast<int64_t>(part * 100ULL / whole);
}

int64_t timeOfDayPercent(const ReadingTimeBucket bucket) {
  const uint64_t* buckets = READING_STATS.getTimeOfDayMs();
  uint64_t total = 0;
  for (size_t i = 0; i < READING_TIME_BUCKET_COUNT; i++) total += buckets[i];
  return percentOf(buckets[static_cast<size_t>(bucket)], total);
}

bool weekMet(const uint8_t dayIndex, int64_t& out) {
  uint8_t mask = 0;
  if (!currentWeekGoalMask(mask)) return false;
  out = (mask & (1u << dayIndex)) ? 100 : 0;
  return true;
}

void copyText(const char* text, char* out, const size_t outSize) { snprintf(out, outSize, "%s", text ? text : ""); }

static_assert(static_cast<int>(Id::Recent10Cover) - static_cast<int>(Id::Recent1Title) + 1 ==
                  RECENT_SLOTS * RECENT_FIELDS,
              "recent.N values must be contiguous, four per slot");

// recent.N.<field> -> slot N-1 and field (0 title, 1 author, 2 progress, 3 cover).
bool recentSlot(const Id id, uint8_t& slot, uint8_t& field) {
  const int v = static_cast<int>(id) - static_cast<int>(Id::Recent1Title);
  if (v < 0 || v >= RECENT_SLOTS * RECENT_FIELDS) return false;
  slot = static_cast<uint8_t>(v / RECENT_FIELDS);
  field = static_cast<uint8_t>(v % RECENT_FIELDS);
  return true;
}

// The recents list as the home screen shows it: most recent first, books whose
// file is gone skipped. One exists() per entry, so it is cached per frame.
void resolveRecents(uint8_t* idx, uint8_t& count) {
  count = 0;
  const auto& books = RECENT_BOOKS.getBooks();
  for (size_t i = 0; i < books.size() && i <= UINT8_MAX && count < RECENT_SLOTS; i++) {
    if (RecentBooksStore::isMissing(books[i])) continue;
    idx[count++] = static_cast<uint8_t>(i);
  }
}

uint8_t recentCount(const RecentBook** slotBook = nullptr, const uint8_t slot = 0) {
  uint8_t localIdx[RECENT_SLOTS];
  const uint8_t* idx = localIdx;
  uint8_t count = 0;
  if (cache) {
    if (!cache->recentsDone) {
      cache->recentsDone = true;
      resolveRecents(cache->recentIdx, cache->recentCount);
    }
    idx = cache->recentIdx;
    count = cache->recentCount;
  } else {
    resolveRecents(localIdx, count);
  }
  if (slotBook != nullptr) *slotBook = slot < count ? &RECENT_BOOKS.getBooks()[idx[slot]] : nullptr;
  return count;
}

const RecentBook* recentBook(const uint8_t slot) {
  const RecentBook* book = nullptr;
  recentCount(&book, slot);
  return book;
}

// A recent book's title, or its file name when the store has none (a book
// whose metadata was never cached).
void recentTitle(const RecentBook& book, char* out, const size_t outSize) {
  if (!book.title.empty()) return copyText(book.title.c_str(), out, outSize);
  const size_t slash = book.path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  size_t end = book.path.find_last_of('.');
  if (end == std::string::npos || end < start) end = book.path.size();
  snprintf(out, outSize, "%.*s", static_cast<int>(end - start), book.path.c_str() + start);
}

void formatDuration(const uint64_t ms, char* out, const size_t outSize) {
  copyText(ReadingStatsAnalytics::formatDurationHm(ms).c_str(), out, outSize);
}

// Number with an optional all-zeros pattern: "000" pads 7 to "007".
void formatNumber(const int64_t value, const char* fmt, const size_t fmtLen, char* out, const size_t outSize) {
  int width = 0;
  for (size_t i = 0; i < fmtLen && fmt[i] == '0'; i++) width++;
  snprintf(out, outSize, "%0*lld", width, static_cast<long long>(value));
}

// Date/time pattern: YYYY YY MM M DD D HH H hh h mm ss A; anything else is
// copied. Empty when the clock was never set.
void formatTimestamp(const uint32_t epochSeconds, const char* fmt, size_t fmtLen, char* out, const size_t outSize,
                     const char* defaultFmt) {
  out[0] = '\0';
  if (epochSeconds == 0 || !TimeUtils::isClockValid(epochSeconds)) return;
  if (fmt == nullptr || fmtLen == 0) {
    fmt = defaultFmt;
    fmtLen = strlen(defaultFmt);
  }
  TimeUtils::configureTimezone();
  const time_t t = static_cast<time_t>(epochSeconds);
  struct tm lt = {};
  localtime_r(&t, &lt);

  size_t o = 0;
  auto put = [&](const char* s) {
    while (*s && o + 1 < outSize) out[o++] = *s++;
  };
  char num[8];
  size_t i = 0;
  auto startsWith = [&](const char* token) {
    const size_t n = strlen(token);
    return i + n <= fmtLen && strncmp(fmt + i, token, n) == 0;
  };
  while (i < fmtLen && o + 1 < outSize) {
    const int hour12 = lt.tm_hour % 12 == 0 ? 12 : lt.tm_hour % 12;
    // Unicode (LDML) patterns: yyyy yy MM M dd d HH H hh h mm ss a. The
    // uppercase YYYY YY DD D A of the first theme format still mean year, day
    // and AM/PM (week-year and day-of-year aren't supported).
    if (startsWith("yyyy") || startsWith("YYYY")) {
      snprintf(num, sizeof(num), "%04d", lt.tm_year + 1900);
      i += 4;
    } else if (startsWith("yy") || startsWith("YY")) {
      snprintf(num, sizeof(num), "%02d", (lt.tm_year + 1900) % 100);
      i += 2;
    } else if (startsWith("MM")) {
      snprintf(num, sizeof(num), "%02d", lt.tm_mon + 1);
      i += 2;
    } else if (startsWith("M")) {
      snprintf(num, sizeof(num), "%d", lt.tm_mon + 1);
      i += 1;
    } else if (startsWith("dd") || startsWith("DD")) {
      snprintf(num, sizeof(num), "%02d", lt.tm_mday);
      i += 2;
    } else if (startsWith("d") || startsWith("D")) {
      snprintf(num, sizeof(num), "%d", lt.tm_mday);
      i += 1;
    } else if (startsWith("HH")) {
      snprintf(num, sizeof(num), "%02d", lt.tm_hour);
      i += 2;
    } else if (startsWith("H")) {
      snprintf(num, sizeof(num), "%d", lt.tm_hour);
      i += 1;
    } else if (startsWith("hh")) {
      snprintf(num, sizeof(num), "%02d", hour12);
      i += 2;
    } else if (startsWith("h")) {
      snprintf(num, sizeof(num), "%d", hour12);
      i += 1;
    } else if (startsWith("mm")) {
      snprintf(num, sizeof(num), "%02d", lt.tm_min);
      i += 2;
    } else if (startsWith("ss")) {
      snprintf(num, sizeof(num), "%02d", lt.tm_sec);
      i += 2;
    } else if (startsWith("a") || startsWith("A")) {
      snprintf(num, sizeof(num), "%s", lt.tm_hour < 12 ? "AM" : "PM");
      i += 1;
    } else {
      num[0] = fmt[i++];
      num[1] = '\0';
    }
    put(num);
  }
  out[o] = '\0';
}

bool timestampOf(const Id id, uint32_t& out) {
  const ReadingBookStats* book = currentBook();
  switch (id) {
    case Id::BookStarted:
      out = book ? book->firstReadAt : 0;
      return out != 0;
    case Id::BookFinished:
      out = (book && book->completed) ? book->completedAt : 0;
      return out != 0;
    case Id::TimeNow:
      out = TimeUtils::getCurrentValidTimestamp();
      return out != 0;
    default:
      return false;
  }
}

void text(const Id id, char* out, const size_t outSize) {
  out[0] = '\0';
  const ReadingBookStats* book = currentBook();
  uint64_t ms = 0;
  switch (id) {
    case Id::StatsTotalTime:
      return formatDuration(READING_STATS.getTotalReadingMs(), out, outSize);
    case Id::StatsTodayTime:
      return formatDuration(READING_STATS.getTodayReadingMs(), out, outSize);
    case Id::StatsWeekTime:
      return formatDuration(READING_STATS.getRecentReadingMs(7), out, outSize);
    case Id::StatsMonthTime:
      return formatDuration(READING_STATS.getRecentReadingMs(30), out, outSize);
    case Id::StatsDailyAvgTime:
      return formatDuration(READING_STATS.getRecentReadingMs(7) / 7, out, outSize);
    case Id::StatsAvgSessionTime:
      return formatDuration(READING_STATS.getAverageSessionMs(), out, outSize);
    case Id::StatsLongestSessionTime:
      return formatDuration(READING_STATS.getLongestSessionMs(), out, outSize);
    case Id::StatsGoalLeftTime: {
      const uint64_t today = READING_STATS.getTodayReadingMs();
      const uint64_t goal = SETTINGS.getDailyGoalMs();
      return formatDuration(today >= goal ? 0 : goal - today, out, outSize);
    }
    case Id::StatsTopAuthor1:
    case Id::StatsTopAuthor2:
    case Id::StatsTopAuthor3: {
      const auto* lib = library();
      const uint8_t idx = static_cast<uint8_t>(id) - static_cast<uint8_t>(Id::StatsTopAuthor1);
      if (lib && idx < lib->authorCount) copyText(lib->authors[idx].name, out, outSize);
      return;
    }
    case Id::StatsTopTag1:
    case Id::StatsTopTag2:
    case Id::StatsTopTag3: {
      const auto* lib = library();
      const uint8_t idx = static_cast<uint8_t>(id) - static_cast<uint8_t>(Id::StatsTopTag1);
      if (lib && idx < lib->tagCount) copyText(lib->tags[idx].name, out, outSize);
      return;
    }
    case Id::BookTitle:
      if (book) copyText(book->title.c_str(), out, outSize);
      return;
    case Id::BookAuthor:
      if (book) copyText(book->author.c_str(), out, outSize);
      return;
    case Id::BookChapter:
      if (book) copyText(book->chapterTitle.c_str(), out, outSize);
      return;
    case Id::BookTime:
      if (book) formatDuration(book->totalReadingMs, out, outSize);
      return;
    case Id::BookTimeLeft:
      if (estimateBookTimeLeftMs(ms)) formatDuration(ms, out, outSize);
      return;
    case Id::DeviceContact:
      copyText(SETTINGS.returnContact, out, outSize);
      return;
    case Id::DeviceModel:
#if FREEINK_DEVICE_X4PRO
      copyText("X4 Pro", out, outSize);
#elif FREEINK_DEVICE_PICCO
      copyText("Picco", out, outSize);
#else
      copyText(gpio.deviceIsX3() ? "X3" : "X4", out, outSize);
#endif
      return;
    case Id::DeviceFirmware:
      copyText(CROSSPOINT_VERSION, out, outSize);
      return;
    case Id::TimeZone: {
      char label[TimeZoneRegistry::OFFSET_BUFFER_SIZE];
      copyText(TimeUtils::getCurrentTimeZoneLabel(label, sizeof(label)), out, outSize);
      return;
    }
    default: {
      uint8_t slot = 0, field = 0;
      if (!recentSlot(id, slot, field)) return;
      const RecentBook* recent = recentBook(slot);
      if (recent == nullptr) return;
      if (field == 0) recentTitle(*recent, out, outSize);
      if (field == 1) copyText(recent->author.c_str(), out, outSize);
      return;
    }
  }
}

}  // namespace

const Info& info(const Id id) { return INFOS[static_cast<size_t>(id)]; }

bool find(const char* name, const size_t nameLen, Id& out) {
  for (size_t i = 0; i < static_cast<size_t>(Id::Count); i++) {
    if (strlen(INFOS[i].name) == nameLen && strncmp(INFOS[i].name, name, nameLen) == 0) {
      out = static_cast<Id>(i);
      return true;
    }
  }
  return false;
}

bool timestamp(const Id id, uint32_t& out) { return timestampOf(id, out); }

void beginFrame() {
  cache = makeUniqueNoThrow<FrameCache>();
  if (!cache) LOG_ERR("STH", "malloc failed: %u bytes value cache", static_cast<unsigned>(sizeof(FrameCache)));
  SleepThemePluginVars::beginFrame();
}

void endFrame() {
  SleepThemePluginVars::endFrame();
  cache.reset();
}

bool number(const Id id, int64_t& out) {
  const ReadingBookStats* book = currentBook();
  const uint64_t goalMs = SETTINGS.getDailyGoalMs();
  const uint64_t todayMs = READING_STATS.getTodayReadingMs();
  uint64_t ms = 0;
  uint32_t days = 0;
  uint8_t mask = 0;
  switch (id) {
    case Id::StatsTotalHours:
      out = static_cast<int64_t>(READING_STATS.getTotalReadingMs() / (60 * MS_PER_MINUTE));
      return true;
    case Id::StatsTotalMinutes:
      out = static_cast<int64_t>(READING_STATS.getTotalReadingMs() / MS_PER_MINUTE);
      return true;
    case Id::StatsTodayMinutes:
      out = static_cast<int64_t>(todayMs / MS_PER_MINUTE);
      return true;
    case Id::StatsWeekHours:
      out = static_cast<int64_t>(READING_STATS.getRecentReadingMs(7) / (60 * MS_PER_MINUTE));
      return true;
    case Id::StatsWeekMinutes:
      out = static_cast<int64_t>(READING_STATS.getRecentReadingMs(7) / MS_PER_MINUTE);
      return true;
    case Id::StatsMonthHours:
      out = static_cast<int64_t>(READING_STATS.getRecentReadingMs(30) / (60 * MS_PER_MINUTE));
      return true;
    case Id::StatsMonthMinutes:
      out = static_cast<int64_t>(READING_STATS.getRecentReadingMs(30) / MS_PER_MINUTE);
      return true;
    case Id::StatsDailyAvgMinutes:
      out = static_cast<int64_t>(READING_STATS.getRecentReadingMs(7) / 7 / MS_PER_MINUTE);
      return true;
    case Id::StatsStreak:
      out = READING_STATS.getCurrentStreakDays();
      return true;
    case Id::StatsMaxStreak:
      out = READING_STATS.getMaxStreakDays();
      return true;
    case Id::StatsSessions:
      out = READING_STATS.getTotalSessionCount();
      return true;
    case Id::StatsSessionsToday:
      out = READING_STATS.getSessionsToday();
      return true;
    case Id::StatsAvgSessionMinutes:
      out = static_cast<int64_t>(READING_STATS.getAverageSessionMs() / MS_PER_MINUTE);
      return true;
    case Id::StatsLongestSessionMinutes:
      out = static_cast<int64_t>(READING_STATS.getLongestSessionMs() / MS_PER_MINUTE);
      return true;
    case Id::StatsBooksStarted:
      out = READING_STATS.getBooksStartedCount();
      return true;
    case Id::StatsBooksFinished:
      out = READING_STATS.getBooksFinishedCount();
      return true;
    case Id::StatsGoalMinutes:
      out = static_cast<int64_t>(goalMs / MS_PER_MINUTE);
      return true;
    case Id::StatsGoalPercent:
      out = todayMs >= goalMs ? 100 : percentOf(todayMs, goalMs);
      return true;
    case Id::StatsGoalLeftMinutes:
      out = todayMs >= goalMs ? 0 : static_cast<int64_t>((goalMs - todayMs + MS_PER_MINUTE - 1) / MS_PER_MINUTE);
      return true;
    case Id::StatsGoalMet:
      out = todayMs >= goalMs ? 1 : 0;
      return true;
    case Id::StatsDaysThisMonth:
      if (!daysReadThisMonth(days)) return false;
      out = days;
      return true;
    case Id::StatsWeekGoalDays:
      if (!currentWeekGoalMask(mask)) return false;
      out = __builtin_popcount(mask);
      return true;
    case Id::StatsWeekMet1:
    case Id::StatsWeekMet2:
    case Id::StatsWeekMet3:
    case Id::StatsWeekMet4:
    case Id::StatsWeekMet5:
    case Id::StatsWeekMet6:
    case Id::StatsWeekMet7:
      return weekMet(static_cast<uint8_t>(id) - static_cast<uint8_t>(Id::StatsWeekMet1), out);
    case Id::StatsMorningPercent:
      out = timeOfDayPercent(ReadingTimeBucket::Morning);
      return true;
    case Id::StatsAfternoonPercent:
      out = timeOfDayPercent(ReadingTimeBucket::Afternoon);
      return true;
    case Id::StatsEveningPercent:
      out = timeOfDayPercent(ReadingTimeBucket::Evening);
      return true;
    case Id::StatsNightPercent:
      out = timeOfDayPercent(ReadingTimeBucket::Night);
      return true;
    case Id::StatsFinishedMonth:
      if (library() == nullptr || !library()->datesKnown) return false;
      out = library()->finishedThisMonth;
      return true;
    case Id::StatsFinishedYear:
      if (library() == nullptr || !library()->datesKnown) return false;
      out = library()->finishedThisYear;
      return true;
    case Id::StatsProfileScore:
      if (profile() == nullptr) return false;
      out = profile()->totalScore;
      return true;
    case Id::StatsProfileHabit:
      if (profile() == nullptr) return false;
      out = profile()->habit.score;
      return true;
    case Id::StatsProfileStability:
      if (profile() == nullptr) return false;
      out = profile()->stability.score;
      return true;
    case Id::StatsProfileEngagement:
      if (profile() == nullptr) return false;
      out = profile()->engagement.score;
      return true;
    case Id::StatsProfileDepth:
      if (profile() == nullptr) return false;
      out = profile()->depth.score;
      return true;
    case Id::BookProgress:
      if (!book) return false;
      out = book->lastProgressPercent;
      return true;
    case Id::BookChapterProgress:
      if (!book) return false;
      out = book->chapterProgressPercent;
      return true;
    case Id::BookTimeMinutes:
      if (!book) return false;
      out = static_cast<int64_t>(book->totalReadingMs / MS_PER_MINUTE);
      return true;
    case Id::BookSessions:
      if (!book) return false;
      out = book->sessions;
      return true;
    case Id::BookTimeLeftMinutes:
      if (!estimateBookTimeLeftMs(ms)) return false;
      out = static_cast<int64_t>(ms / MS_PER_MINUTE);
      return true;
    case Id::BookDaysReading:
      if (!book) return false;
      out = static_cast<int64_t>(book->readingDays.size());
      return true;
    case Id::BookClippings:
      if (APP_STATE.openEpubPath.empty() || !cache) return false;
      if (!cache->clippingsDone) {
        cache->clippingsDone = true;
        cache->clippings = ClippingStore::countForFilePath(APP_STATE.openEpubPath, "epub");
      }
      out = cache->clippings;
      return true;
    case Id::DeviceBattery:
      out = powerManager.getBatteryPercentage();
      return true;
    case Id::DeviceCharging:
      out = gpio.isUsbConnectedCached() ? 1 : 0;
      return true;
    case Id::RecentCount:
      out = recentCount();
      return true;
    default: {
      uint8_t slot = 0, field = 0;
      if (!recentSlot(id, slot, field) || field != 2) return false;
      const RecentBook* recent = recentBook(slot);
      if (recent == nullptr || recent->progressPercent < 0) return false;
      out = recent->progressPercent;
      return true;
    }
  }
}

bool recentCoverPath(const Id id, std::string& out) {
  uint8_t slot = 0, field = 0;
  if (!recentSlot(id, slot, field) || field != 3) return false;
  const RecentBook* recent = recentBook(slot);
  if (recent == nullptr) {
    LOG_INF("STH", "recent.%u.cover: no book in that slot", slot + 1);
    return false;
  }
  if (recent->coverBmpPath.empty()) {
    LOG_INF("STH", "recent.%u.cover: %s has no cover path", slot + 1, recent->path.c_str());
    return false;
  }
  // coverBmpPath is a thumbnail template inside the book's cache folder.
  const size_t slash = recent->coverBmpPath.find_last_of('/');
  if (slash == std::string::npos) return false;
  const std::string dir = recent->coverBmpPath.substr(0, slash);

  // The full cover exists once the book has been a Cover sleep screen.
  std::string full = dir + "/cover.bmp";
  if (Storage.exists(full.c_str())) {
    out = std::move(full);
    return true;
  }
  // Otherwise the largest thumbnail: the home screen and the Bookshelf theme
  // cache them at their own sizes (thumb_<h>.bmp, thumb_<key>_<w>x<h>.bmp).
  auto folder = Storage.open(dir.c_str());
  if (!folder || !folder.isDirectory()) return false;
  folder.rewindDirectory();
  char name[64];
  size_t bestSize = 0;
  std::string best;
  for (auto entry = folder.openNextFile(); entry; entry = folder.openNextFile()) {
    entry.getName(name, sizeof(name));
    const size_t len = strlen(name);
    if (entry.isDirectory() || len < 10 || strncmp(name, "thumb_", 6) != 0 || strcasecmp(name + len - 4, ".bmp") != 0) {
      continue;
    }
    const size_t size = entry.fileSize();
    if (size > bestSize) {
      bestSize = size;
      best = name;
    }
  }
  if (best.empty()) {
    LOG_INF("STH", "recent.%u.cover: no cover.bmp or thumbnail in %s yet", slot + 1, dir.c_str());
    return false;
  }
  out = dir + "/" + best;
  return true;
}

void format(const Id id, const char* fmt, const size_t fmtLen, char* out, const size_t outSize) {
  out[0] = '\0';
  if (outSize == 0) return;
  switch (info(id).kind) {
    case Kind::Number: {
      int64_t value = 0;
      if (number(id, value)) formatNumber(value, fmt, fmtLen, out, outSize);
      return;
    }
    case Kind::Timestamp: {
      uint32_t ts = 0;
      if (timestampOf(id, ts))
        formatTimestamp(ts, fmt, fmtLen, out, outSize, id == Id::TimeNow ? "HH:mm" : "yyyy-MM-dd");
      return;
    }
    case Kind::Text:
      text(id, out, outSize);
      return;
    case Kind::Image:
      return;
  }
}

void formatTime(const uint32_t epochSeconds, const char* fmt, const size_t fmtLen, char* out, const size_t outSize) {
  formatTimestamp(epochSeconds, fmt, fmtLen, out, outSize, "yyyy-MM-dd");
}

namespace {
// The "}}" closing the block opened at p, skipping any inside a quoted
// string ({{x ? "}}" : ""}}). Null when there is none.
const char* blockEnd(const char* p) {
  char quote = 0;
  for (; *p; p++) {
    if (quote) {
      if (*p == '\\' && p[1]) {
        p++;
      } else if (*p == quote) {
        quote = 0;
      }
    } else if (*p == '"' || *p == '\'') {
      quote = *p;
    } else if (p[0] == '}' && p[1] == '}') {
      return p;
    }
  }
  return nullptr;
}
}  // namespace

bool expand(const char* tpl, char* out, const size_t outSize) {
  if (outSize == 0) return false;
  size_t o = 0;
  bool allPresent = true;
  char value[VALUE_LEN];
  const char* p = tpl;
  while (*p && o + 1 < outSize) {
    if (p[0] == '{' && p[1] == '{') {
      const char* end = blockEnd(p + 2);
      if (end != nullptr && !SleepThemeExpr::isPlainValue(p + 2, static_cast<size_t>(end - (p + 2)))) {
        bool present = false;
        SleepThemeExpr::evaluate(p + 2, static_cast<size_t>(end - (p + 2)), value, sizeof(value), present);
        if (!present) allPresent = false;
        for (const char* v = value; *v && o + 1 < outSize; v++) out[o++] = *v;
        p = end + 2;
        continue;
      }
      if (end != nullptr) {
        const char* name = p + 2;
        const char* colon = static_cast<const char*>(memchr(name, ':', static_cast<size_t>(end - name)));
        const size_t nameLen = static_cast<size_t>((colon ? colon : end) - name);
        const char* fmt = colon ? colon + 1 : nullptr;
        const size_t fmtLen = colon ? static_cast<size_t>(end - fmt) : 0;
        Id id;
        if (find(name, nameLen, id)) {
          format(id, fmt, fmtLen, value, sizeof(value));
        } else if (SleepThemePluginVars::isPluginVar(name, nameLen)) {
          // Straight into out, not through value: a plugin's text may run past
          // VALUE_LEN, up to what the element itself can hold. No format applies.
          const size_t before = o;
          SleepThemePluginVars::lookup(name, nameLen, out + o, outSize - o);
          o += strlen(out + o);
          if (o == before) allPresent = false;
          p = end + 2;
          continue;
        } else {
          LOG_ERR("STH", "Unknown variable in template: %.*s", static_cast<int>(nameLen), name);
          value[0] = '\0';
        }
        if (value[0] == '\0') allPresent = false;
        for (const char* v = value; *v && o + 1 < outSize; v++) out[o++] = *v;
        p = end + 2;
        continue;
      }
    }
    out[o++] = *p++;
  }
  out[o] = '\0';
  return allPresent;
}

}  // namespace SleepThemeVars
