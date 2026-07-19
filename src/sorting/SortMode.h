#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

enum class SortMode : uint8_t {
  AlphabeticAsc,
  AlphabeticDesc,
  AuthorAsc,
  AuthorDesc,
  LastOpenedNewest,
  LastOpenedOldest,
  ProgressMost,
  ProgressLeast,
  DateAddedNewest,
  DateAddedOldest,
  BookFusionFirst,
  BookFusionLast,
  TagAsc,
  TagDesc,
  // Numeric series order (book 1, 2, 3…). Appended last so existing persisted
  // SortMode values (0–13) keep their meaning. Only the grouped Series browse
  // offers it; the flat list activities never do. Requires SortEntry::seriesIndex.
  SeriesIndexAsc,
};

constexpr uint8_t SORT_MODE_COUNT = 15;

// A selectable sort field for the sort menu: a primary (default) direction and its reverse.
// The menu shows one row per field; Confirm toggles primary<->reverse. When a field has no
// distinct reverse (e.g. series order), reverse == primary and the toggle is a no-op.
struct SortOption {
  SortMode primary;
  SortMode reverse;
};

// The full field set the flat list activities (Library / Recents / FileBrowser) offer.
// Excludes SeriesIndexAsc, which needs per-book series metadata those views don't hold.
inline constexpr SortOption ALL_SORT_OPTIONS[] = {
    {SortMode::AlphabeticAsc, SortMode::AlphabeticDesc},
    {SortMode::AuthorAsc, SortMode::AuthorDesc},
    {SortMode::LastOpenedNewest, SortMode::LastOpenedOldest},
    {SortMode::ProgressMost, SortMode::ProgressLeast},
    {SortMode::DateAddedNewest, SortMode::DateAddedOldest},
    {SortMode::BookFusionFirst, SortMode::BookFusionLast},
    {SortMode::TagAsc, SortMode::TagDesc},
};
inline constexpr int ALL_SORT_OPTIONS_COUNT = 7;

struct SortEntry {
  std::string_view sortKey;
  std::string_view authorKey;
  std::string_view tagKey;
  int8_t progressPercent = -1;
  uint16_t lastOpenedRank = 0xFFFF;
  uint32_t dateAddedTs = 0;
  bool hasBfBadge = false;
  // Numeric series position for SeriesIndexAsc; < 0 means "no index" (sorts to the end).
  float seriesIndex = -1.0f;
};

// Sentinel for "never opened" — items with this rank sort to the end of LastOpened* modes.
constexpr uint16_t LAST_OPENED_NEVER = 0xFFFF;

// Fill `indices` with [0, entries.size()) permuted per `mode`. Items missing the primary
// criterion (empty authorKey, empty tagKey, progress < 0, rank == LAST_OPENED_NEVER,
// dateAddedTs == 0) sort to the end. Ties break case-insensitively on sortKey for stable,
// deterministic order.
void applySort(std::vector<uint16_t>& indices, const std::vector<SortEntry>& entries, SortMode mode);

// Returns a translated label suitable for the sort-picker UI.
const char* sortModeLabel(SortMode m);

SortMode sortModeNext(SortMode m);
