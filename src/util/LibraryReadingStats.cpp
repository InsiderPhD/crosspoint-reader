#include "LibraryReadingStats.h"

#include <Arduino.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "ReadingStatsStore.h"
#include "activities/home/LibraryScan.h"
#include "util/KeyTotals.h"
#include "util/TimeUtils.h"

namespace {
constexpr char MODULE[] = "LIBSTATS";
constexpr uint16_t NO_ORDINAL = 0xFFFF;
// Distinct authors + tags tracked in one pass. 320 x 16 bytes = 5 KB, enough
// for a few hundred read books; past it new names are dropped (and logged)
// while the totals of names already seen stay exact.
constexpr uint16_t MAX_BUCKETS = 320;
// Names of one book checked against each other so a subject listed twice in an
// EPUB is not credited twice. 8 bytes each on the stack.
constexpr uint8_t MAX_SEEN_PER_BOOK = 16;

// Same split the Tags/Authors browser uses (GroupBrowserActivity::splitKeys):
// subjects are ", "-joined, co-authors are '&'-separated ("Last, First" keeps
// its comma).
constexpr char AUTHOR_DELIMITER = '&';
constexpr char TAG_DELIMITER = ',';

// One path a stats book is known by, hashed the way the index hashes paths.
struct PathKey {
  uint64_t hash;
  uint16_t bookIndex;
};

struct PathLookup {
  const PathKey* keys;
  size_t keyCount;
  uint16_t* ordinals;  // per stats book; NO_ORDINAL until found
  size_t unresolved;

  static bool match(void* context, const uint16_t ordinal, const uint64_t hash) {
    auto& lookup = *static_cast<PathLookup*>(context);
    const PathKey* end = lookup.keys + lookup.keyCount;
    const PathKey* it =
        std::lower_bound(lookup.keys, end, hash, [](const PathKey& key, const uint64_t h) { return key.hash < h; });
    for (; it != end && it->hash == hash; ++it) {
      if (lookup.ordinals[it->bookIndex] == NO_ORDINAL) {
        lookup.ordinals[it->bookIndex] = ordinal;
        lookup.unresolved--;
      }
    }
    return lookup.unresolved > 0;  // stop the scan once every book is placed
  }
};

uint64_t hashPath(const std::string& path) { return library::clixPathHash(path.data(), path.size()); }

bool hasReadingTime(const ReadingBookStats& book) { return book.totalReadingMs >= 1000 && !book.path.empty(); }

void countFinished(LibraryReadingStats::Summary& out) {
  const uint32_t today = TimeUtils::getLocalDayOrdinal(READING_STATS.getDisplayTimestamp());
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (today == 0 || !TimeUtils::getDateFromDayOrdinal(today, year, month, day)) {
    return;
  }
  out.datesKnown = true;
  for (const ReadingBookStats& book : READING_STATS.getBooks()) {
    // A book finished while the clock was unset has no date and cannot be
    // placed in a month; it still counts in the lifetime Books Finished row.
    if (!book.completed || !TimeUtils::isClockValid(book.completedAt)) continue;
    int finishedYear = 0;
    unsigned finishedMonth = 0;
    unsigned finishedDay = 0;
    // Local calendar date of the finish, in the device's one time zone.
    const uint32_t finished = TimeUtils::getLocalDayOrdinal(book.completedAt);
    if (finished == 0 || !TimeUtils::getDateFromDayOrdinal(finished, finishedYear, finishedMonth, finishedDay)) {
      continue;
    }
    if (finishedYear != year) continue;
    out.finishedThisYear++;
    if (finishedMonth == month) out.finishedThisMonth++;
  }
}

// Credit `seconds` to every distinct name in one book's list.
void creditNames(KeyTotals::Table& table, const KeyTotals::Kind kind, const std::string& list, const char delimiter,
                 const uint32_t seconds, const uint16_t ordinal) {
  uint64_t seen[MAX_SEEN_PER_BOOK];
  uint8_t seenCount = 0;
  size_t cursor = 0;
  uint8_t tokenIndex = 0;
  KeyTotals::Token token;
  while (KeyTotals::nextToken(list.data(), list.size(), delimiter, cursor, token)) {
    const uint64_t key = KeyTotals::makeKey(kind, token.data, token.len);
    const bool duplicate = std::find(seen, seen + seenCount, key) != seen + seenCount;
    if (!duplicate) {
      if (seenCount < MAX_SEEN_PER_BOOK) seen[seenCount++] = key;
      table.add(key, seconds, ordinal, tokenIndex);
    }
    if (tokenIndex == UINT8_MAX) break;
    tokenIndex++;
  }
}

// Read back the spelling a bucket points at and copy it out for display.
bool readName(library::LibraryIndexFile& index, const KeyTotals::Bucket& bucket, std::string& scratch, char* out) {
  const bool isAuthor = KeyTotals::kindOf(bucket) == KeyTotals::KIND_AUTHOR;
  library::ClixRecord record{};
  if (!index.readRecord(bucket.ordinal, record)) return false;
  if (!(isAuthor ? index.readAuthor(record, scratch) : index.readGenre(record, scratch))) return false;

  const char delimiter = isAuthor ? AUTHOR_DELIMITER : TAG_DELIMITER;
  size_t cursor = 0;
  KeyTotals::Token token;
  for (uint8_t i = 0;; i++) {
    if (!KeyTotals::nextToken(scratch.data(), scratch.size(), delimiter, cursor, token)) return false;
    if (i == bucket.token) break;
  }
  const int wanted = static_cast<int>(std::min(token.len, LibraryReadingStats::NAME_BYTES - 1));
  const int length = utf8SafeTruncateBuffer(token.data, wanted);
  if (length <= 0) return false;
  memcpy(out, token.data, static_cast<size_t>(length));
  out[length] = '\0';
  return true;
}

uint8_t fillTop(library::LibraryIndexFile& index, const KeyTotals::Table& table, const KeyTotals::Kind kind,
                std::string& scratch, LibraryReadingStats::NamedTotal* out) {
  KeyTotals::Bucket best[LibraryReadingStats::TOP_COUNT];  // 80 bytes
  const uint8_t found = table.top(kind, best, LibraryReadingStats::TOP_COUNT);
  uint8_t count = 0;
  for (uint8_t i = 0; i < found; i++) {
    if (!readName(index, best[i], scratch, out[count].name)) continue;
    out[count].readingSeconds = best[i].seconds;
    out[count].books = best[i].books;
    count++;
  }
  return count;
}

void computeTopLists(LibraryReadingStats::Summary& out) {
  if (!LibraryScan::indexEnabled()) return;
  const auto& books = READING_STATS.getBooks();
  const size_t bookCount = std::min<size_t>(books.size(), NO_ORDINAL);

  // The index as it stands on the card. A stale one is still the best join
  // available, and rebuilding it is the Library's job (it can take minutes and
  // needs a cancel UI), not something to start from a stats tab.
  library::LibraryIndexFile index;
  if (!index.open(LibraryScan::indexPath())) {
    LOG_INF(MODULE, "No usable Library index (%s)", library::clixValidityName(index.validity()));
    return;
  }
  out.indexAvailable = true;
  if (bookCount == 0 || index.bookCount() == 0) return;

  // 1. Where each stats book sits in the index. A book is looked up by every
  //    path it has been known by, so one moved since the last scan still joins.
  size_t keyCount = 0;
  size_t wanted = 0;
  for (size_t i = 0; i < bookCount; i++) {
    if (!hasReadingTime(books[i])) continue;
    wanted++;
    keyCount += 1 + books[i].knownPaths.size();
  }
  if (keyCount == 0) return;

  // Flat arrays sized exactly once: 2 bytes per stats book for the whole
  // computation, 16 bytes per known path only until the index scan is done.
  auto ordinals = makeUniqueNoThrow<uint16_t[]>(bookCount);
  auto keys = makeUniqueNoThrow<PathKey[]>(keyCount);
  if (!ordinals || !keys) {
    LOG_ERR(MODULE, "No memory to join %u books to the index", static_cast<unsigned>(bookCount));
    return;
  }
  std::fill(ordinals.get(), ordinals.get() + bookCount, NO_ORDINAL);
  size_t filled = 0;
  for (size_t i = 0; i < bookCount; i++) {
    const ReadingBookStats& book = books[i];
    if (!hasReadingTime(book)) continue;
    keys[filled++] = {hashPath(book.path), static_cast<uint16_t>(i)};
    for (const std::string& known : book.knownPaths) {
      keys[filled++] = {hashPath(known), static_cast<uint16_t>(i)};
    }
  }
  std::sort(keys.get(), keys.get() + filled, [](const PathKey& a, const PathKey& b) { return a.hash < b.hash; });

  PathLookup lookup{keys.get(), filled, ordinals.get(), wanted};
  const bool scanned = index.forEachPathHash(&PathLookup::match, &lookup);
  keys.reset();  // returned before the bucket table is allocated
  if (!scanned) {
    LOG_ERR(MODULE, "Could not read the Library index's path hashes");
    return;
  }

  // 2. Reading time per author and per tag, in one pass over the joined books.
  auto buckets = makeUniqueNoThrow<KeyTotals::Bucket[]>(MAX_BUCKETS);
  if (!buckets) {
    LOG_ERR(MODULE, "No memory for the author/tag totals");
    return;
  }
  KeyTotals::Table table(buckets.get(), MAX_BUCKETS);
  std::string scratch;
  scratch.reserve(255);  // longest blob field the index stores
  unsigned joined = 0;
  for (size_t i = 0; i < bookCount; i++) {
    const uint16_t ordinal = ordinals[i];
    if (ordinal == NO_ORDINAL) continue;
    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record)) continue;
    const uint32_t seconds = static_cast<uint32_t>(std::min<uint64_t>(books[i].totalReadingMs / 1000ULL, UINT32_MAX));
    if (index.readAuthor(record, scratch)) {
      creditNames(table, KeyTotals::KIND_AUTHOR, scratch, AUTHOR_DELIMITER, seconds, ordinal);
    }
    if (index.readGenre(record, scratch)) {
      creditNames(table, KeyTotals::KIND_TAG, scratch, TAG_DELIMITER, seconds, ordinal);
    }
    joined++;
    if ((joined & 0x3Fu) == 0) delay(1);  // keep the watchdog fed on a long history
  }
  if (table.droppedCount() > 0) {
    LOG_INF(MODULE, "Author/tag table full: %u names not counted", static_cast<unsigned>(table.droppedCount()));
  }

  // 3. Only the names that will be drawn are read back as text.
  out.authorCount = fillTop(index, table, KeyTotals::KIND_AUTHOR, scratch, out.authors);
  out.tagCount = fillTop(index, table, KeyTotals::KIND_TAG, scratch, out.tags);
  LOG_DBG(MODULE, "Joined %u of %u read books to the index; %u names", joined, static_cast<unsigned>(wanted),
          static_cast<unsigned>(table.size()));
}
}  // namespace

void LibraryReadingStats::compute(Summary& out) {
  out = Summary{};
  // The one place a "reading stats off" switch would return early: both parts
  // below only ever read ReadingStatsStore.
  countFinished(out);
  computeTopLists(out);
}
