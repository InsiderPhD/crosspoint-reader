#include "ReadingStatsAnalytics.h"

#include <algorithm>
#include <array>
#include <ctime>

#include "util/TimeUtils.h"

namespace ReadingStatsAnalytics {
namespace {
constexpr uint64_t MIN_READING_DAY_BOOK_MS = 3ULL * 60ULL * 1000ULL;

// Session-length bucket thresholds (milliseconds), matching cpr-vcodex.
constexpr uint32_t TEN_MINUTES_MS = 10U * 60U * 1000U;
constexpr uint32_t THIRTY_MINUTES_MS = 30U * 60U * 1000U;
constexpr int PROFILE_WINDOW_DAYS = 7;

int roundDiv(const int numerator, const int denominator) {
  if (denominator == 0) {
    return 0;
  }
  return (numerator + denominator / 2) / denominator;
}

int clampPercent(const int value) { return std::clamp(value, 0, 100); }

std::string formatFraction(const int value, const int total) {
  return std::to_string(value) + "/" + std::to_string(total);
}

std::string formatPercentLabel(const int value) { return std::to_string(clampPercent(value)) + "%"; }

std::string formatTenths(const int tenths) { return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10); }

// Trailing-window anchor: today if the clock is valid, otherwise the most
// recent recorded reading day so the profile still renders offline.
uint32_t getProfileReferenceDayOrdinal() {
  const uint32_t timestamp = READING_STATS.getDisplayTimestamp();
  if (TimeUtils::isClockValid(timestamp)) {
    return TimeUtils::getLocalDayOrdinal(timestamp);
  }
  const auto& days = READING_STATS.getReadingDays();
  if (!days.empty()) {
    return days.back().dayOrdinal;
  }
  return 0;
}

int resolveYearFromTimestamp(const uint32_t timestamp) {
  if (!TimeUtils::isClockValid(timestamp)) {
    return 0;
  }

  time_t currentTime = static_cast<time_t>(timestamp);
  tm localTime = {};
  if (localtime_r(&currentTime, &localTime) == nullptr) {
    return 0;
  }
  return localTime.tm_year + 1900;
}

}  // namespace

std::string formatDurationHm(const uint64_t totalMs) {
  const uint64_t totalMinutes = totalMs / 60000ULL;
  const uint64_t hours = totalMinutes / 60ULL;
  const uint64_t minutes = totalMinutes % 60ULL;
  if (hours == 0) {
    return std::to_string(minutes) + "m";
  }
  return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
}

std::string formatDayOrdinalLabel(const uint32_t dayOrdinal) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (!TimeUtils::getDateFromDayOrdinal(dayOrdinal, year, month, day)) {
    return "";
  }

  return TimeUtils::formatDateParts(year, month, day);
}

std::string formatMonthLabel(const int year, const unsigned month) { return TimeUtils::formatMonthYear(year, month); }

int getReferenceYear() {
  const uint32_t timestamp = READING_STATS.getDisplayTimestamp();
  if (const int year = resolveYearFromTimestamp(timestamp); year != 0) {
    return year;
  }

  if (!READING_STATS.getReadingDays().empty()) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (TimeUtils::getDateFromDayOrdinal(READING_STATS.getReadingDays().back().dayOrdinal, year, month, day)) {
      return year;
    }
  }

  return 2026;
}

std::vector<DayBookEntry> getBooksReadOnDay(const uint32_t dayOrdinal) {
  std::vector<DayBookEntry> entries;
  for (const auto& book : READING_STATS.getBooks()) {
    auto it = std::find_if(book.readingDays.begin(), book.readingDays.end(), [&](const ReadingDayStats& day) {
      return day.dayOrdinal == dayOrdinal && day.readingMs >= MIN_READING_DAY_BOOK_MS;
    });
    if (it == book.readingDays.end()) {
      continue;
    }

    entries.push_back(DayBookEntry{&book, it->readingMs});
  }

  std::sort(entries.begin(), entries.end(), [](const DayBookEntry& left, const DayBookEntry& right) {
    if (left.readingMs != right.readingMs) {
      return left.readingMs > right.readingMs;
    }
    if (!left.book || !right.book) {
      return left.book != nullptr;
    }
    return left.book->title < right.book->title;
  });
  return entries;
}

TimelineDayEntry buildTimelineDayEntry(const uint32_t dayOrdinal) {
  TimelineDayEntry entry;
  entry.dayOrdinal = dayOrdinal;
  for (const auto& day : READING_STATS.getReadingDays()) {
    if (day.dayOrdinal == dayOrdinal) {
      entry.totalReadingMs = day.readingMs;
      break;
    }
  }

  const auto books = getBooksReadOnDay(dayOrdinal);
  entry.booksReadCount = static_cast<uint32_t>(books.size());
  if (!books.empty()) {
    entry.topBook = books.front().book;
    entry.topBookReadingMs = books.front().readingMs;
  }
  return entry;
}

std::vector<TimelineDayEntry> buildTimelineEntries(const size_t maxEntries) {
  std::vector<TimelineDayEntry> entries;
  const auto& readingDays = READING_STATS.getReadingDays();
  entries.reserve(readingDays.size());

  for (auto it = readingDays.rbegin(); it != readingDays.rend(); ++it) {
    if (it->readingMs == 0) {
      continue;
    }
    entries.push_back(buildTimelineDayEntry(it->dayOrdinal));
    if (maxEntries > 0 && entries.size() >= maxEntries) {
      break;
    }
  }
  return entries;
}

SessionBuckets countSessionDurationBuckets() {
  SessionBuckets buckets;
  for (const auto& session : READING_STATS.getSessionLog()) {
    buckets.total++;
    if (session.sessionMs < TEN_MINUTES_MS) {
      buckets.under10++;
    } else if (session.sessionMs < THIRTY_MINUTES_MS) {
      buckets.mid++;
    } else {
      buckets.over30++;
    }
  }
  return buckets;
}

ReadingProfileSummary buildReadingProfileSummary() {
  ReadingProfileSummary summary;

  const uint32_t referenceDayOrdinal = getProfileReferenceDayOrdinal();
  if (referenceDayOrdinal == 0) {
    return summary;  // hasData stays false
  }

  const uint32_t startDayOrdinal = referenceDayOrdinal >= static_cast<uint32_t>(PROFILE_WINDOW_DAYS - 1)
                                       ? referenceDayOrdinal - (PROFILE_WINDOW_DAYS - 1)
                                       : 0;
  const uint64_t dailyGoalMs = getDailyReadingGoalMs();

  // Fold the trailing 7 aggregate reading days into a fixed-size window.
  std::array<uint64_t, PROFILE_WINDOW_DAYS> readingMsByDay = {};
  const auto& readingDays = READING_STATS.getReadingDays();
  for (auto it = readingDays.rbegin(); it != readingDays.rend(); ++it) {
    if (it->dayOrdinal < startDayOrdinal) {
      break;
    }
    if (it->dayOrdinal > referenceDayOrdinal) {
      continue;
    }
    const size_t index = static_cast<size_t>(it->dayOrdinal - startDayOrdinal);
    if (index < readingMsByDay.size()) {
      readingMsByDay[index] += it->readingMs;
    }
  }

  int daysRead = 0;
  int goalDays = 0;
  int longestReadStreak = 0;
  int currentGoalStreak = 0;
  uint64_t weeklyTotalReadingMs = 0;
  uint64_t maxDayReadingMs = 0;
  for (const uint64_t readingMs : readingMsByDay) {
    weeklyTotalReadingMs += readingMs;
    maxDayReadingMs = std::max(maxDayReadingMs, readingMs);
    if (readingMs > 0) {
      daysRead++;
      if (readingMs >= dailyGoalMs) {
        goalDays++;
        currentGoalStreak++;
        longestReadStreak = std::max(longestReadStreak, currentGoalStreak);
      } else {
        currentGoalStreak = 0;
      }
    } else {
      currentGoalStreak = 0;
    }
  }

  int bestDaySharePercent = 0;
  if (weeklyTotalReadingMs > 0) {
    bestDaySharePercent = roundDiv(static_cast<int>(maxDayReadingMs * 100ULL), static_cast<int>(weeklyTotalReadingMs));
  }

  // Session-length mix over the same trailing window (session log is dated).
  int sessions = 0;
  int sessionsUnder10m = 0;
  int sessions10to29m = 0;
  const auto& sessionLog = READING_STATS.getSessionLog();
  for (auto it = sessionLog.rbegin(); it != sessionLog.rend(); ++it) {
    if (it->dayOrdinal != 0 && it->dayOrdinal < startDayOrdinal) {
      break;
    }
    if (it->dayOrdinal != 0 && it->dayOrdinal > referenceDayOrdinal) {
      continue;
    }
    if (it->dayOrdinal == 0) {
      continue;  // undated sessions can't be placed in the window
    }
    sessions++;
    if (it->sessionMs < TEN_MINUTES_MS) {
      sessionsUnder10m++;
    } else if (it->sessionMs < THIRTY_MINUTES_MS) {
      sessions10to29m++;
    }
  }

  // Fallback for the clock-was-invalid case: if the window has reading days but
  // no dated sessions, treat each day's total as a single synthetic session so
  // engagement/depth don't collapse to zero.
  if (sessions == 0 && daysRead > 0) {
    for (const uint64_t readingMs : readingMsByDay) {
      if (readingMs == 0) {
        continue;
      }
      sessions++;
      if (readingMs < TEN_MINUTES_MS) {
        sessionsUnder10m++;
      } else if (readingMs < THIRTY_MINUTES_MS) {
        sessions10to29m++;
      }
    }
  }

  int sessionsPerReadDayTenths = 0;
  if (daysRead > 0) {
    sessionsPerReadDayTenths = roundDiv(sessions * 10, daysRead);
  }
  int under10Percent = 0;
  int midPercent = 0;
  int over30Percent = 0;
  if (sessions > 0) {
    under10Percent = roundDiv(sessionsUnder10m * 100, sessions);
    midPercent = roundDiv(sessions10to29m * 100, sessions);
    if (under10Percent + midPercent > 100) {
      if (midPercent >= under10Percent) {
        midPercent = 100 - under10Percent;
      } else {
        under10Percent = 100 - midPercent;
      }
    }
    over30Percent = clampPercent(100 - under10Percent - midPercent);
  }

  const int habitScore = clampPercent(roundDiv(daysRead * 65 + goalDays * 35, PROFILE_WINDOW_DAYS));
  const int streakScore = goalDays > 0 ? roundDiv(longestReadStreak * 100, goalDays) : 0;
  int balanceScore = 0;
  if (daysRead > 1 && weeklyTotalReadingMs > 0) {
    const double bestShare = static_cast<double>(maxDayReadingMs) / static_cast<double>(weeklyTotalReadingMs);
    const double idealShare = 1.0 / static_cast<double>(daysRead);
    const double normalized = 1.0 - ((bestShare - idealShare) / (1.0 - idealShare));
    balanceScore = clampPercent(static_cast<int>(normalized * 100.0 + 0.5));
  }
  const int stabilityScore = clampPercent((streakScore + balanceScore + 1) / 2);
  const int sessionsScore = std::min(100, sessions * 10);
  const int sessionsPerReadDayScore = daysRead > 0 ? std::min(100, roundDiv(sessions * 100, daysRead * 3)) : 0;
  const int engagementScore = clampPercent((sessionsScore * 60 + sessionsPerReadDayScore * 40 + 50) / 100);
  const int depthScore = clampPercent(roundDiv(midPercent * 50 + over30Percent * 100, 100));

  summary.hasData = true;
  summary.totalScore = clampPercent((habitScore + stabilityScore + engagementScore + depthScore + 2) / 4);

  summary.habit.score = habitScore;
  summary.habit.primaryValue = formatFraction(daysRead, PROFILE_WINDOW_DAYS);
  summary.habit.secondaryValue = formatFraction(goalDays, PROFILE_WINDOW_DAYS);

  summary.stability.score = stabilityScore;
  summary.stability.primaryValue = std::to_string(longestReadStreak) + "d";
  summary.stability.secondaryValue = formatPercentLabel(bestDaySharePercent);

  summary.engagement.score = engagementScore;
  summary.engagement.primaryValue = std::to_string(sessions);
  summary.engagement.secondaryValue = formatTenths(sessionsPerReadDayTenths);

  summary.depth.score = depthScore;
  summary.depth.primaryValue = formatPercentLabel(under10Percent);
  summary.depth.secondaryValue = formatPercentLabel(midPercent);
  summary.depth.tertiaryValue = formatPercentLabel(over30Percent);

  return summary;
}

}  // namespace ReadingStatsAnalytics
