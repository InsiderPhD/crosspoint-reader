#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Live values a sleep theme can show. Templates reference them by name:
//   "{{stats.total_hours}}"          plain value
//   "{{stats.streak:000}}"           numbers: zero-pad to the format's length
//   "{{time.now:dd.MM.yyyy HH:mm}}"  timestamps: Unicode date/time pattern
//                                    (yyyy yy MM M dd d HH H hh h mm ss a;
//                                    mm is minutes, MM the month)
// The same table drives the browser compiler (via GET /api/sleep-theme/vars),
// so a name the device does not know is rejected at compile time.
namespace SleepThemeVars {

enum class Kind : uint8_t {
  Number,     // integer, formatted with an optional zero-pad pattern
  Text,       // UTF-8 string
  Timestamp,  // epoch seconds, formatted with a date/time pattern
  Image,      // only valid on a Cover element (book.cover, recent.N.cover, sleep.image)
};

// X(id, name, kind, needsScan). needsScan marks values that read the SD card
// (Library index) or walk the whole stats store; they are computed once per
// frame, on first use only.
// The recently opened books, most recent first (the home screen's recents
// list, skipping books whose file is gone). Four values per slot, kept
// together so a slot's fields are contiguous ids: see recentSlot().
#define SLEEP_THEME_RECENT(X, n)                                  \
  X(Recent##n##Title, "recent." #n ".title", Text, false)         \
  X(Recent##n##Author, "recent." #n ".author", Text, false)       \
  X(Recent##n##Progress, "recent." #n ".progress", Number, false) \
  X(Recent##n##Cover, "recent." #n ".cover", Image, false)

#define SLEEP_THEME_VARS(X)                                                     \
  X(StatsTotalHours, "stats.total_hours", Number, false)                        \
  X(StatsTotalMinutes, "stats.total_minutes", Number, false)                    \
  X(StatsTotalTime, "stats.total_time", Text, false)                            \
  X(StatsTodayMinutes, "stats.today_minutes", Number, false)                    \
  X(StatsTodayTime, "stats.today_time", Text, false)                            \
  X(StatsWeekHours, "stats.week_hours", Number, false)                          \
  X(StatsWeekMinutes, "stats.week_minutes", Number, false)                      \
  X(StatsWeekTime, "stats.week_time", Text, false)                              \
  X(StatsMonthHours, "stats.month_hours", Number, false)                        \
  X(StatsMonthMinutes, "stats.month_minutes", Number, false)                    \
  X(StatsMonthTime, "stats.month_time", Text, false)                            \
  X(StatsDailyAvgMinutes, "stats.daily_avg_minutes", Number, false)             \
  X(StatsDailyAvgTime, "stats.daily_avg_time", Text, false)                     \
  X(StatsStreak, "stats.streak", Number, false)                                 \
  X(StatsMaxStreak, "stats.max_streak", Number, false)                          \
  X(StatsSessions, "stats.sessions", Number, false)                             \
  X(StatsSessionsToday, "stats.sessions_today", Number, false)                  \
  X(StatsAvgSessionMinutes, "stats.avg_session_minutes", Number, false)         \
  X(StatsAvgSessionTime, "stats.avg_session_time", Text, false)                 \
  X(StatsLongestSessionMinutes, "stats.longest_session_minutes", Number, false) \
  X(StatsLongestSessionTime, "stats.longest_session_time", Text, false)         \
  X(StatsBooksStarted, "stats.books_started", Number, false)                    \
  X(StatsBooksFinished, "stats.books_finished", Number, false)                  \
  X(StatsGoalMinutes, "stats.goal_minutes", Number, false)                      \
  X(StatsGoalPercent, "stats.goal_percent", Number, false)                      \
  X(StatsGoalLeftMinutes, "stats.goal_left_minutes", Number, false)             \
  X(StatsGoalLeftTime, "stats.goal_left_time", Text, false)                     \
  X(StatsGoalMet, "stats.goal_met", Number, false)                              \
  X(StatsDaysThisMonth, "stats.days_this_month", Number, false)                 \
  X(StatsWeekGoalDays, "stats.week_goal_days", Number, false)                   \
  X(StatsWeekMet1, "stats.week_met_1", Number, false)                           \
  X(StatsWeekMet2, "stats.week_met_2", Number, false)                           \
  X(StatsWeekMet3, "stats.week_met_3", Number, false)                           \
  X(StatsWeekMet4, "stats.week_met_4", Number, false)                           \
  X(StatsWeekMet5, "stats.week_met_5", Number, false)                           \
  X(StatsWeekMet6, "stats.week_met_6", Number, false)                           \
  X(StatsWeekMet7, "stats.week_met_7", Number, false)                           \
  X(StatsMorningPercent, "stats.morning_percent", Number, false)                \
  X(StatsAfternoonPercent, "stats.afternoon_percent", Number, false)            \
  X(StatsEveningPercent, "stats.evening_percent", Number, false)                \
  X(StatsNightPercent, "stats.night_percent", Number, false)                    \
  X(StatsFinishedMonth, "stats.finished_month", Number, true)                   \
  X(StatsFinishedYear, "stats.finished_year", Number, true)                     \
  X(StatsTopAuthor1, "stats.top_author_1", Text, true)                          \
  X(StatsTopAuthor2, "stats.top_author_2", Text, true)                          \
  X(StatsTopAuthor3, "stats.top_author_3", Text, true)                          \
  X(StatsTopTag1, "stats.top_tag_1", Text, true)                                \
  X(StatsTopTag2, "stats.top_tag_2", Text, true)                                \
  X(StatsTopTag3, "stats.top_tag_3", Text, true)                                \
  X(StatsProfileScore, "stats.profile_score", Number, true)                     \
  X(StatsProfileHabit, "stats.profile_habit", Number, true)                     \
  X(StatsProfileStability, "stats.profile_stability", Number, true)             \
  X(StatsProfileEngagement, "stats.profile_engagement", Number, true)           \
  X(StatsProfileDepth, "stats.profile_depth", Number, true)                     \
  X(BookTitle, "book.title", Text, false)                                       \
  X(BookAuthor, "book.author", Text, false)                                     \
  X(BookChapter, "book.chapter", Text, false)                                   \
  X(BookProgress, "book.progress", Number, false)                               \
  X(BookChapterProgress, "book.chapter_progress", Number, false)                \
  X(BookTimeMinutes, "book.time_minutes", Number, false)                        \
  X(BookTime, "book.time", Text, false)                                         \
  X(BookSessions, "book.sessions", Number, false)                               \
  X(BookTimeLeftMinutes, "book.time_left_minutes", Number, false)               \
  X(BookTimeLeft, "book.time_left", Text, false)                                \
  X(BookDaysReading, "book.days_reading", Number, false)                        \
  X(BookStarted, "book.started", Timestamp, false)                              \
  X(BookFinished, "book.finished", Timestamp, false)                            \
  X(BookClippings, "book.clippings", Number, true)                              \
  X(BookCover, "book.cover", Image, false)                                      \
  X(SleepImage, "sleep.image", Image, false)                                    \
  X(TimeNow, "time.now", Timestamp, false)                                      \
  X(DeviceBattery, "device.battery", Number, false)                             \
  X(DeviceContact, "device.contact", Text, false)                               \
  X(DeviceModel, "device.model", Text, false)                                   \
  X(DeviceFirmware, "device.firmware", Text, false)                             \
  X(DeviceCharging, "device.charging", Number, false)                           \
  X(TimeZone, "time.zone", Text, false)                                         \
  X(RecentCount, "recent.count", Number, false)                                 \
  SLEEP_THEME_RECENT(X, 1)                                                      \
  SLEEP_THEME_RECENT(X, 2)                                                      \
  SLEEP_THEME_RECENT(X, 3)                                                      \
  SLEEP_THEME_RECENT(X, 4)                                                      \
  SLEEP_THEME_RECENT(X, 5)                                                      \
  SLEEP_THEME_RECENT(X, 6)                                                      \
  SLEEP_THEME_RECENT(X, 7)                                                      \
  SLEEP_THEME_RECENT(X, 8)                                                      \
  SLEEP_THEME_RECENT(X, 9)                                                      \
  SLEEP_THEME_RECENT(X, 10)

enum class Id : uint8_t {
#define SLEEP_THEME_VAR_ENUM(id, name, kind, scan) id,
  SLEEP_THEME_VARS(SLEEP_THEME_VAR_ENUM)
#undef SLEEP_THEME_VAR_ENUM
      Count
};

inline constexpr uint8_t RECENT_SLOTS = 10;
inline constexpr uint8_t RECENT_FIELDS = 4;  // title, author, progress, cover

struct Info {
  const char* name;
  Kind kind;
  bool needsScan;
};

inline constexpr size_t VALUE_LEN = 96;

const Info& info(Id id);
// Name lookup. Returns false when the name is unknown.
bool find(const char* name, size_t nameLen, Id& out);

// Opens a frame: allocates the per-frame caches (Library scan, profile,
// current book) that values share. Pair with endFrame(), which frees them.
// Values resolved outside a frame still work; the SD-scan ones read as unknown.
void beginFrame();
void endFrame();

// Formats one value into out (always NUL-terminated). An empty result means
// "unknown right now" (no book open, clock not set, ...). fmt may be null.
void format(Id id, const char* fmt, size_t fmtLen, char* out, size_t outSize);

// Raw numeric value for Bar elements and digit slots; false when unknown.
bool number(Id id, int64_t& out);
// Raw epoch seconds of a Timestamp value; false when unknown.
bool timestamp(Id id, uint32_t& out);

// Path of the cached cover image for a recent.N.cover value: the book's full
// cover.bmp when one exists, else its largest cached thumbnail. Never opens
// the book itself, so it is safe at sleep time. False when there is none.
bool recentCoverPath(Id id, std::string& out);

// Formats epoch seconds with a date/time pattern (see the comment at the top).
// Empty when the clock isn't set.
void formatTime(uint32_t epochSeconds, const char* fmt, size_t fmtLen, char* out, size_t outSize);

// Expands every {{name[:fmt]}} or {{expression}} (SleepThemeExpr.h) in tpl into out. Text outside braces is copied
// as is. Returns false when any referenced value was empty (used by
// hide-if-empty); unknown names expand to nothing.
bool expand(const char* tpl, char* out, size_t outSize);

}  // namespace SleepThemeVars
