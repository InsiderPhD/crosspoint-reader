#include "BookmarkUtil.h"

#include <algorithm>
#include <cctype>

std::string BookmarkUtil::getBookmarksDir() { return "/.crosspoint/bookmarks/"; }

std::string BookmarkUtil::getBookmarkPath(const std::string& bookPath) {
  // Create a flat filename under /.crosspoint/bookmarks so books in different
  // folders still get stable, distinct bookmark files.
  std::string bookName = bookPath;
  if (!bookName.empty() && bookName[0] == '/') {
    bookName.erase(0, 1);
  }
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');

  const size_t lastDot = bookName.find_last_of('.');
  if (lastDot != std::string::npos) {
    bookName.erase(lastDot);
  }
  bookName += ".json";
  return getBookmarksDir() + bookName;
}

std::string BookmarkUtil::sanitizeBookmarkSummary(std::string summary) {
  summary.erase(std::unique(summary.begin(), summary.end(),
                            [](char a, char b) { return std::isspace(a) && std::isspace(b); }),
                summary.end());
  summary.erase(std::remove(summary.begin(), summary.end(), '\n'), summary.end());
  summary.erase(summary.begin(),
                std::find_if(summary.begin(), summary.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  summary.erase(std::find_if(summary.rbegin(), summary.rend(), [](unsigned char ch) { return !std::isspace(ch); })
                    .base(),
                summary.end());

  if (summary.size() > 72) {
    summary.resize(72);
  }
  return summary;
}
