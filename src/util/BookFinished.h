#pragma once

#include <string>

#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"

// "Finished" for list screens (the Library's tile badge and Hide Finished Books
// filter, the file browser's row icon).
//
// A book is finished when the reader marked it completed in ReadingStatsStore
// (it reached READING_COMPLETED_PERCENT), or when "Mark as Read" set its
// RecentBooksStore progress to 100. Both stores are loaded at boot and live in
// RAM, so these checks cost no SD access and add no state of their own.
namespace BookFinished {

inline bool isFinished(const std::string& path) {
  if (path.empty()) return false;
  if (const ReadingBookStats* stats = READING_STATS.findBook(path); stats && stats->completed) return true;
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (book.path == path) return book.progressPercent >= 100;
  }
  return false;
}

// Calls `visit(context, path)` once per finished book either store knows (a
// book in both may be visited twice). Lets a caller that has no paths of its
// own - the Library on its CLX1 index - locate finished books by path hash.
inline void forEachFinishedPath(void (*visit)(void* context, const std::string& path), void* context) {
  for (const ReadingBookStats& book : READING_STATS.getBooks()) {
    if (book.completed && !book.path.empty()) visit(context, book.path);
  }
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (book.progressPercent >= 100) visit(context, book.path);
  }
}

// Upper bound on the number of forEachFinishedPath() visits, for reserve().
inline size_t finishedPathUpperBound() { return READING_STATS.getBooks().size() + RECENT_BOOKS.getBooks().size(); }

}  // namespace BookFinished
