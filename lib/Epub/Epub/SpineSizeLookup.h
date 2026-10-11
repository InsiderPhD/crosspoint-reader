#pragma once

#include <cstddef>

// Cumulative spine sizes are nondecreasing (an empty chapter repeats the previous
// value). Binary-search for the first spine item whose end offset is >= target,
// the same boundary the reader's old linear walk chose, in O(log n) size lookups
// instead of one per preceding chapter. Returns count - 1 when target lies past
// the last item, and -1 for an empty spine. No scratch storage.
template <typename ReadCumulativeSize>
int findSpineForSize(const int count, const size_t target, ReadCumulativeSize&& readSize) {
  if (count <= 0) return -1;
  int first = 0;
  int last = count;
  while (first < last) {
    const int middle = first + (last - first) / 2;
    if (readSize(middle) < target) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first < count ? first : count - 1;
}
