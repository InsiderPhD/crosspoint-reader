#pragma once

#include <cstdint>
#include <string>

// A single bookmark entry representing a position in an EPUB.
struct BookmarkEntry {
  std::string xpath;
  std::string summary;
  float percentage = 0.0f;

  // Cached page mapping for faster bookmark list rendering.
  uint16_t computedSpineIndex = 0;
  uint16_t computedChapterPageCount = 0;
  uint16_t computedChapterProgress = 0;
};
