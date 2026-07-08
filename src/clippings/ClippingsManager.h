#pragma once

#include <string>

// Appends a saved clipping to a Kindle-style "/My Clippings.txt" on the SD card so highlights
// can be exported/read on a computer. Separate from the bounded in-app ClippingStore.
class ClippingsManager {
 public:
  static bool saveClipping(const std::string& bookTitle, const std::string& author, const std::string& chapterTitle,
                           int pageNumber, const std::string& selectedText);

  static constexpr const char* CLIPPINGS_PATH = "/My Clippings.txt";
};
