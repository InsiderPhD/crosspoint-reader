#pragma once

#include <cstddef>
#include <cstdint>

// Reading stats that need the Library's metadata: books finished this month and
// this year, and reading time per author and per tag.
//
// ReadingStatsStore supplies everything about reading (time per book, when a
// book was finished); the CLX1 Library index is only the metadata join - a
// book's author and tags, found by the hash of its path. Nothing here is
// resident: compute() fills one caller-owned Summary (the Stats screen holds it
// while it is open) and frees all of its working memory before returning.
namespace LibraryReadingStats {

inline constexpr uint8_t TOP_COUNT = 5;
// Longest name kept for display, in bytes including the terminator. Names are
// cut on a UTF-8 boundary; the row truncates to its pixel width anyway.
inline constexpr size_t NAME_BYTES = 48;

struct NamedTotal {
  char name[NAME_BYTES] = {};
  uint32_t readingSeconds = 0;
  uint8_t books = 0;
};

struct Summary {
  // False while the device has no trustworthy date: "this month" is undefined.
  bool datesKnown = false;
  uint16_t finishedThisMonth = 0;
  uint16_t finishedThisYear = 0;

  // False when the Library index is off, missing or unreadable; the two lists
  // are then empty and the screen says how to get them.
  bool indexAvailable = false;
  uint8_t authorCount = 0;
  uint8_t tagCount = 0;
  NamedTotal authors[TOP_COUNT];
  NamedTotal tags[TOP_COUNT];
};

// Fill `out`. Reads the index from the SD card (one pass over its path hashes,
// then one record per book that has reading time), so call it on screen entry,
// never per frame. Transient heap, all freed on return: 16 bytes per known book
// path, 2 bytes per stats book, a 5 KB bucket table, and the index reader's own
// 4 KB chunk buffer.
void compute(Summary& out);

}  // namespace LibraryReadingStats
