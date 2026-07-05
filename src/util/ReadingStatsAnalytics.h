#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ReadingStatsStore.h"

namespace ReadingStatsAnalytics {

struct DayBookEntry {
  const ReadingBookStats* book = nullptr;
  uint64_t readingMs = 0;
};

struct TimelineDayEntry {
  uint32_t dayOrdinal = 0;
  uint64_t totalReadingMs = 0;
  uint32_t booksReadCount = 0;
  const ReadingBookStats* topBook = nullptr;
  uint64_t topBookReadingMs = 0;
};

// Session-length distribution over the (bounded) session log. Buckets are
// < 10m / 10-29m / >= 30m. Reflects the last MAX_SESSION_LOG_ENTRIES sessions.
struct SessionBuckets {
  uint32_t under10 = 0;
  uint32_t mid = 0;
  uint32_t over30 = 0;
  uint32_t total = 0;
};

// One axis of the Reading Profile. score is 0-100; the value strings are
// preformatted for display (labels are assigned by the render side).
struct ProfileDimension {
  int score = 0;
  std::string primaryValue;
  std::string secondaryValue;
  std::string tertiaryValue;
};

// Derived reading-behaviour summary over the trailing 7 days. Ports the
// cpr-vcodex scoring model. hasData is false when there is no valid reference
// day / no reading yet.
struct ReadingProfileSummary {
  bool hasData = false;
  int totalScore = 0;
  ProfileDimension habit;       // DaysRead x/7, GoalsMet x/7
  ProfileDimension stability;   // ReadStreak Nd, TopDayWeight %
  ProfileDimension engagement;  // Sessions N, PerReadDay N.N
  ProfileDimension depth;       // <10m %, 10-29m %, >30m %
};

std::string formatDurationHm(uint64_t totalMs);
std::string formatDayOrdinalLabel(uint32_t dayOrdinal);
std::string formatMonthLabel(int year, unsigned month);
int getReferenceYear();
std::vector<DayBookEntry> getBooksReadOnDay(uint32_t dayOrdinal);
TimelineDayEntry buildTimelineDayEntry(uint32_t dayOrdinal);
std::vector<TimelineDayEntry> buildTimelineEntries(size_t maxEntries = 0);
SessionBuckets countSessionDurationBuckets();
ReadingProfileSummary buildReadingProfileSummary();

}  // namespace ReadingStatsAnalytics
