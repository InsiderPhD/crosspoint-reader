#include "SortMode.h"

#include <I18n.h>

#include <algorithm>
#include <cctype>

namespace {

int ciCompare(std::string_view a, std::string_view b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) {
    const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
    const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  return 0;
}

inline bool ciLess(std::string_view a, std::string_view b) { return ciCompare(a, b) < 0; }

// Three-way "missing values go to end + tie-break alphabetically" comparator.
// `cmp` returns <0 when a should come before b on the primary key.
template <typename Cmp>
bool missingLastLess(bool aMissing, bool bMissing, std::string_view aKey, std::string_view bKey, Cmp&& cmp) {
  if (aMissing != bMissing) return !aMissing;  // present items come first
  if (aMissing) return ciLess(aKey, bKey);
  const int c = cmp();
  if (c != 0) return c < 0;
  return ciLess(aKey, bKey);
}

bool compareEntries(SortMode mode, const SortEntry& a, const SortEntry& b) {
  switch (mode) {
    case SortMode::AlphabeticAsc:
      return ciLess(a.sortKey, b.sortKey);
    case SortMode::AlphabeticDesc:
      return ciLess(b.sortKey, a.sortKey);
    case SortMode::AuthorAsc:
      return missingLastLess(a.authorKey.empty(), b.authorKey.empty(), a.sortKey, b.sortKey,
                             [&] { return ciCompare(a.authorKey, b.authorKey); });
    case SortMode::AuthorDesc:
      return missingLastLess(a.authorKey.empty(), b.authorKey.empty(), a.sortKey, b.sortKey,
                             [&] { return ciCompare(b.authorKey, a.authorKey); });
    case SortMode::LastOpenedNewest:
      return missingLastLess(a.lastOpenedRank == LAST_OPENED_NEVER, b.lastOpenedRank == LAST_OPENED_NEVER, a.sortKey,
                             b.sortKey, [&] { return (int)a.lastOpenedRank - (int)b.lastOpenedRank; });
    case SortMode::LastOpenedOldest:
      return missingLastLess(a.lastOpenedRank == LAST_OPENED_NEVER, b.lastOpenedRank == LAST_OPENED_NEVER, a.sortKey,
                             b.sortKey, [&] { return (int)b.lastOpenedRank - (int)a.lastOpenedRank; });
    case SortMode::ProgressMost:
      return missingLastLess(a.progressPercent < 0, b.progressPercent < 0, a.sortKey, b.sortKey,
                             [&] { return (int)b.progressPercent - (int)a.progressPercent; });
    case SortMode::ProgressLeast:
      return missingLastLess(a.progressPercent < 0, b.progressPercent < 0, a.sortKey, b.sortKey,
                             [&] { return (int)a.progressPercent - (int)b.progressPercent; });
    case SortMode::DateAddedNewest:
      return missingLastLess(a.dateAddedTs == 0, b.dateAddedTs == 0, a.sortKey, b.sortKey, [&] {
        return a.dateAddedTs == b.dateAddedTs ? 0 : (a.dateAddedTs > b.dateAddedTs ? -1 : 1);
      });
    case SortMode::DateAddedOldest:
      return missingLastLess(a.dateAddedTs == 0, b.dateAddedTs == 0, a.sortKey, b.sortKey, [&] {
        return a.dateAddedTs == b.dateAddedTs ? 0 : (a.dateAddedTs < b.dateAddedTs ? -1 : 1);
      });
    case SortMode::BookFusionFirst:
      // BF-linked books first (A-Z within group), then non-BF (A-Z within group).
      return missingLastLess(!a.hasBfBadge, !b.hasBfBadge, a.sortKey, b.sortKey, [&] { return 0; });
    case SortMode::BookFusionLast:
      // Non-BF books first (A-Z within group), then BF-linked books (A-Z within group).
      return missingLastLess(a.hasBfBadge, b.hasBfBadge, a.sortKey, b.sortKey, [&] { return 0; });
  }
  return false;
}

}  // namespace

void applySort(std::vector<uint16_t>& indices, const std::vector<SortEntry>& entries, SortMode mode) {
  indices.clear();
  indices.reserve(entries.size());
  for (uint16_t i = 0; i < entries.size(); i++) indices.push_back(i);

  std::sort(indices.begin(), indices.end(),
            [&](uint16_t lhs, uint16_t rhs) { return compareEntries(mode, entries[lhs], entries[rhs]); });
}

const char* sortModeLabel(SortMode m) {
  switch (m) {
    case SortMode::AlphabeticAsc:
      return tr(STR_SORT_ALPHA_ASC);
    case SortMode::AlphabeticDesc:
      return tr(STR_SORT_ALPHA_DESC);
    case SortMode::AuthorAsc:
      return tr(STR_SORT_AUTHOR_ASC);
    case SortMode::AuthorDesc:
      return tr(STR_SORT_AUTHOR_DESC);
    case SortMode::LastOpenedNewest:
      return tr(STR_SORT_LAST_OPENED_NEW);
    case SortMode::LastOpenedOldest:
      return tr(STR_SORT_LAST_OPENED_OLD);
    case SortMode::ProgressMost:
      return tr(STR_SORT_PROGRESS_MOST);
    case SortMode::ProgressLeast:
      return tr(STR_SORT_PROGRESS_LEAST);
    case SortMode::DateAddedNewest:
      return tr(STR_SORT_DATE_ADDED_NEW);
    case SortMode::DateAddedOldest:
      return tr(STR_SORT_DATE_ADDED_OLD);
    case SortMode::BookFusionFirst:
      return tr(STR_SORT_BOOKFUSION_FIRST);
    case SortMode::BookFusionLast:
      return tr(STR_SORT_BOOKFUSION_LAST);
  }
  return "";
}

SortMode sortModeNext(SortMode m) {
  return static_cast<SortMode>((static_cast<uint8_t>(m) + 1) % SORT_MODE_COUNT);
}
