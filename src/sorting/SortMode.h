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
};

constexpr uint8_t SORT_MODE_COUNT = 14;

struct SortEntry {
  std::string_view sortKey;
  std::string_view authorKey;
  std::string_view tagKey;
  int8_t progressPercent = -1;
  uint16_t lastOpenedRank = 0xFFFF;
  uint32_t dateAddedTs = 0;
  bool hasBfBadge = false;
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
