#include "LibrarySort.h"

#include <Logging.h>

#include <algorithm>
#include <memory>

namespace library {
namespace {

uint16_t testRunCapacity = 0;

bool entryLess(const SortEntry& a, const SortEntry& b) {
  const int cmp = memcmp(a.key, b.key, sizeof(a.key));
  if (cmp != 0) return cmp < 0;
  return a.ordinal < b.ordinal;
}

bool unknownKey(const char* key) {
  return std::all_of(key, key + SORT_SEGMENT_BYTES,
                     [](const char value) { return static_cast<uint8_t>(value) == 0xFF; });
}

void service(const SortConfig& config) {
  if (config.service) config.service();
}

bool stopped(const SortConfig& config) { return config.stopped && config.stopped(); }

// Only ties need another read. Each level reloads one segment into the key and
// re-sorts the tied range, so recursion depth is bounded by keyBytes / 12.
bool refineTies(SortEntry* entries, const uint16_t begin, const uint16_t end, const size_t offset,
                const SortConfig& config) {
  if (end - begin < 2 || offset >= config.keyBytes) return true;
  size_t longest = 0;
  for (uint16_t i = begin; i < end; i++) {
    service(config);
    size_t valueBytes = 0;
    if (stopped(config) || !config.load(config.loadContext, entries[i].source, offset, entries[i].key, valueBytes))
      return false;
    longest = std::max(longest, valueBytes);
  }
  std::sort(entries + begin, entries + end, entryLess);
  // Past the longest value every segment is zero padding, so nothing deeper
  // can separate these entries.
  const size_t nextOffset = offset + SORT_SEGMENT_BYTES;
  if (nextOffset >= longest) return true;
  uint16_t run = begin;
  while (run < end) {
    uint16_t next = run + 1;
    while (next < end && memcmp(entries[run].key, entries[next].key, SORT_SEGMENT_BYTES) == 0) next++;
    if (!refineTies(entries, run, next, nextOffset, config)) return false;
    run = next;
  }
  return true;
}

}  // namespace

void setSortRunCapacityForTesting(const uint16_t entries) { testRunCapacity = entries; }

bool sortEntries(SortEntry* entries, const uint16_t count, const SortConfig& config) {
  if (count > 1) std::sort(entries, entries + count, entryLess);
  if (config.keyBytes <= SORT_SEGMENT_BYTES || !config.load) return true;
  uint16_t run = 0;
  while (run < count) {
    uint16_t next = run + 1;
    while (next < count && memcmp(entries[run].key, entries[next].key, SORT_SEGMENT_BYTES) == 0) next++;
    if (next - run > 1 && !unknownKey(entries[run].key)) {
      // Refinement overwrites keys with later segments. Restore the shared
      // prefix so a spilled run can still be merged on it.
      char prefix[SORT_SEGMENT_BYTES];
      memcpy(prefix, entries[run].key, sizeof(prefix));
      if (!refineTies(entries, run, next, SORT_SEGMENT_BYTES, config)) return false;
      for (uint16_t i = run; i < next; i++) memcpy(entries[i].key, prefix, sizeof(prefix));
    }
    run = next;
  }
  return true;
}

ExternalSorter::~ExternalSorter() { release(); }

void ExternalSorter::release() {
  buffer.reset();
  if (runs) {
    runs.close();
    Storage.remove(config.runPath);
  }
  capacity = 0;
  used = 0;
  runCount = 0;
  total = 0;
}

bool ExternalSorter::begin(const SortConfig& sortConfig, const uint16_t expected) {
  release();
  config = sortConfig;
  failed = false;
  const uint16_t wanted = expected == 0 ? 1 : expected;
  const uint16_t limit = testRunCapacity != 0 ? testRunCapacity : libraryPsramAvailable() ? wanted : SORT_RUN_ENTRIES;
  capacity = std::min(wanted, limit);
  while (!buffer.allocate(capacity)) {
    if (capacity <= SORT_MIN_RUN_ENTRIES) {
      LOG_ERR("LIBSORT", "sort buffer alloc failed (%u bytes)", static_cast<unsigned>(capacity * sizeof(SortEntry)));
      failed = true;
      return false;
    }
    capacity = std::max<uint16_t>(capacity / 2, SORT_MIN_RUN_ENTRIES);
    LOG_INF("LIBSORT", "retrying sort with a %u-entry buffer", static_cast<unsigned>(capacity));
  }
  return true;
}

bool ExternalSorter::add(const SortEntry& entry) {
  if (failed) return false;
  if (used == capacity && !spillRun()) return false;
  buffer[used++] = entry;
  total++;
  return true;
}

bool ExternalSorter::spillRun() {
  if (!sortEntries(buffer.get(), used, config)) {
    failed = true;
    return false;
  }
  if (!runs) {
    runs = Storage.open(config.runPath, O_RDWR | O_CREAT | O_TRUNC);
    if (!runs) {
      LOG_ERR("LIBSORT", "cannot create %s", config.runPath);
      failed = true;
      return false;
    }
  }
  const size_t bytes = static_cast<size_t>(used) * sizeof(SortEntry);
  if (runs.write(buffer.get(), bytes) != bytes) {
    LOG_ERR("LIBSORT", "run write failed (%u bytes)", static_cast<unsigned>(bytes));
    failed = true;
    return false;
  }
  runCount++;
  used = 0;
  service(config);
  return true;
}

// Key bytes cached per merge head after the 12-byte prefix: enough for long
// shared values such as BISAC subjects ("FICTION / Science Fiction / ...") or
// series names, so a long tie costs a few loads per entry, not per comparison.
// Many runs only happen after the sort buffer shrank on a low heap, so the
// cache shrinks with them and stays near 1.5 KiB either way.
constexpr size_t HEAD_CACHED_SEGMENTS = 8;
constexpr uint16_t HEAD_CACHE_FULL_RUNS = 16;

struct ExternalSorter::Head {
  uint32_t next;  // next unread entry of this run in the run file
  uint32_t end;
  uint16_t sliceStart;
  uint16_t count;
  uint16_t position;
  // Segments after the prefix for the current entry, loaded the first time
  // it ties and kept until the head advances, plus the full key length.
  uint8_t cachedSegments;
  uint8_t cacheSegments;  // capacity of `cache`, in segments
  char* cache;
  size_t valueBytes;
  const SortEntry* entry;
};

// The key segment at `offset` (a multiple of 12, at least 12) for `head`,
// from its cache when in range; deeper segments load into `scratch`.
const char* ExternalSorter::segmentAt(Head& head, const size_t offset, char* scratch) {
  const size_t index = offset / SORT_SEGMENT_BYTES - 1;
  if (index >= head.cacheSegments) {
    size_t ignored = 0;
    if (!config.load(config.loadContext, head.entry->source, offset, scratch, ignored)) {
      failed = true;
      return nullptr;
    }
    return scratch;
  }
  while (head.cachedSegments <= index) {
    const size_t at = (head.cachedSegments + 1u) * SORT_SEGMENT_BYTES;
    size_t valueBytes = 0;
    if (!config.load(config.loadContext, head.entry->source, at, head.cache + head.cachedSegments * SORT_SEGMENT_BYTES,
                     valueBytes)) {
      failed = true;
      return nullptr;
    }
    if (head.cachedSegments == 0) head.valueBytes = valueBytes;
    head.cachedSegments++;
  }
  return head.cache + index * SORT_SEGMENT_BYTES;
}

int ExternalSorter::compare(Head& a, Head& b) {
  const SortEntry& left = *a.entry;
  const SortEntry& right = *b.entry;
  const int cmp = memcmp(left.key, right.key, sizeof(left.key));
  if (cmp != 0) return cmp;
  if (config.keyBytes > SORT_SEGMENT_BYTES && config.load && left.source != right.source && !unknownKey(left.key)) {
    char leftScratch[SORT_SEGMENT_BYTES];
    char rightScratch[SORT_SEGMENT_BYTES];
    // The first load also reports each value's length.
    if (!segmentAt(a, SORT_SEGMENT_BYTES, leftScratch) || !segmentAt(b, SORT_SEGMENT_BYTES, rightScratch)) return 0;
    const size_t longest = std::min(config.keyBytes, std::max(a.valueBytes, b.valueBytes));
    for (size_t offset = SORT_SEGMENT_BYTES; offset < longest; offset += SORT_SEGMENT_BYTES) {
      const char* leftSegment = segmentAt(a, offset, leftScratch);
      const char* rightSegment = segmentAt(b, offset, rightScratch);
      if (!leftSegment || !rightSegment) return 0;
      const int segment = memcmp(leftSegment, rightSegment, SORT_SEGMENT_BYTES);
      if (segment != 0) return segment;
    }
  }
  return left.ordinal < right.ordinal ? -1 : (left.ordinal > right.ordinal ? 1 : 0);
}

bool ExternalSorter::merge(const SortEmitter emit, void* const context) {
  const uint16_t slice = capacity / runCount;
  if (slice == 0) {
    LOG_ERR("LIBSORT", "%u runs do not fit a %u-entry buffer", static_cast<unsigned>(runCount),
            static_cast<unsigned>(capacity));
    return false;
  }
  const uint8_t cacheSegments = runCount <= HEAD_CACHE_FULL_RUNS ? HEAD_CACHED_SEGMENTS : 1;
  auto heads = makeUniqueNoThrow<Head[]>(runCount);
  auto caches = makeUniqueNoThrow<char[]>(static_cast<size_t>(runCount) * cacheSegments * SORT_SEGMENT_BYTES);
  if (!heads || !caches) {
    LOG_ERR("LIBSORT", "merge head alloc failed (%u runs)", static_cast<unsigned>(runCount));
    return false;
  }
  const auto refill = [this, &heads, slice](const uint16_t run) {
    Head& head = heads[run];
    head.count = static_cast<uint16_t>(std::min<uint32_t>(slice, head.end - head.next));
    head.position = 0;
    head.cachedSegments = 0;
    head.entry = buffer.get() + head.sliceStart;
    if (head.count == 0) return true;
    const size_t bytes = static_cast<size_t>(head.count) * sizeof(SortEntry);
    if (!runs.seekSet(static_cast<size_t>(head.next) * sizeof(SortEntry)) ||
        runs.read(buffer.get() + head.sliceStart, bytes) != static_cast<int>(bytes)) {
      LOG_ERR("LIBSORT", "run read failed at entry %u", static_cast<unsigned>(head.next));
      return false;
    }
    head.next += head.count;
    return true;
  };
  for (uint16_t run = 0; run < runCount; run++) {
    heads[run] = Head{};
    heads[run].cacheSegments = cacheSegments;
    heads[run].cache = caches.get() + static_cast<size_t>(run) * cacheSegments * SORT_SEGMENT_BYTES;
    heads[run].next = static_cast<uint32_t>(run) * capacity;
    heads[run].end = std::min<uint32_t>(heads[run].next + capacity, total);
    heads[run].sliceStart = static_cast<uint16_t>(run * slice);
    if (!refill(run)) return false;
  }

  // Run counts stay small (16 at the format ceiling with the default buffer),
  // so a linear scan of the heads beats maintaining a heap.
  for (uint32_t emitted = 0; emitted < total; emitted++) {
    service(config);
    if (stopped(config)) return false;
    int best = -1;
    for (uint16_t run = 0; run < runCount; run++) {
      Head& head = heads[run];
      if (head.position >= head.count) continue;
      if (best < 0 || compare(head, heads[best]) < 0) best = run;
      if (failed) return false;
    }
    if (best < 0) {
      LOG_ERR("LIBSORT", "runs ended after %u of %u entries", static_cast<unsigned>(emitted),
              static_cast<unsigned>(total));
      return false;
    }
    Head& head = heads[best];
    if (!emit(context, *head.entry)) return false;
    head.cachedSegments = 0;
    if (++head.position < head.count) {
      head.entry++;
    } else if (head.next < head.end && !refill(static_cast<uint16_t>(best))) {
      return false;
    }
  }
  return true;
}

bool ExternalSorter::finish(const SortEmitter emit, void* const context) {
  bool ok = !failed;
  if (ok && runCount == 0) {
    ok = sortEntries(buffer.get(), used, config);
    for (uint16_t i = 0; ok && i < used; i++) {
      service(config);
      ok = !stopped(config) && emit(context, buffer[i]);
    }
  } else if (ok) {
    ok = (used == 0 || spillRun()) && merge(emit, context);
  }
  if (!ok) failed = true;
  release();
  return ok;
}

}  // namespace library
