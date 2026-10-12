#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// A single bookmark entry representing a position in an EPUB.
struct BookmarkEntry {
  // Byte cap on a user-given name. About as long as a list row can show; kept
  // short because the bookmark list is loaded while a book is open.
  static constexpr std::size_t MAX_NAME_LENGTH = 64;

  std::string xpath;
  std::string summary;
  // Optional label typed by the user; the list shows it in place of the page
  // summary. Empty for a bookmark that was never renamed.
  std::string name;
  float percentage = 0.0f;

  // Cached page mapping for faster bookmark list rendering.
  uint16_t computedSpineIndex = 0;
  uint16_t computedChapterPageCount = 0;
  uint16_t computedChapterProgress = 0;
};
