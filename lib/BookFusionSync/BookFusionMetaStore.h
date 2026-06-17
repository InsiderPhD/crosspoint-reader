#pragma once
#include <string>

// BookFusion organisational metadata that is NOT embedded in the EPUB file
// (categories / bookshelves / lists). Persisted at download time as a sidecar
// inside the book's cache directory so the Book Details view can show it.
struct BookFusionMeta {
  std::string categories;   // ", "-joined
  std::string bookshelves;  // ", "-joined
  std::string lists;        // ", "-joined

  bool empty() const { return categories.empty() && bookshelves.empty() && lists.empty(); }
};

// Reads/writes <cacheDir>/bf_meta.bin (version byte + three length-prefixed
// UTF-8 strings). cacheDir is the book's cache directory, i.e. Epub::getCachePath().
class BookFusionMetaStore {
 public:
  static bool save(const std::string& cacheDir, const BookFusionMeta& meta);
  // Returns false (and leaves out empty) when no sidecar exists or on read error.
  static bool load(const std::string& cacheDir, BookFusionMeta& out);
};
