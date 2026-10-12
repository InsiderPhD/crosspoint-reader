#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

// The publisher's page list: the page numbers of the print edition, as the EPUB
// declares them (EPUB 3 <nav epub:type="page-list">, EPUB 2 NCX <pageList>).
//
// It is kept out of book.bin on purpose. A long book lists several hundred
// pages, almost nobody jumps by page number, and book.bin is read on every
// open. Instead the list is streamed to <cache dir>/pagelist.bin while the TOC
// is parsed (no in-memory copy at any point) and read back one fixed-size
// record at a time, by seek, only while the Go to Page popup is open. A book
// with no page list has no file, and the reader's whole resident cost is the
// entry count in a uint16_t.
//
// File: u8 version, u8 reserved, u16 count, then count records of RECORD_SIZE:
//   char label[LABEL_SIZE]    NUL padded ("12", "xiv", "A-3")
//   u16  spineIndex           little-endian
//   char anchor[ANCHOR_SIZE]  NUL padded fragment id, empty = top of the file
// Records are in document order.
namespace PageList {

constexpr uint8_t FILE_VERSION = 1;
constexpr size_t LABEL_SIZE = 12;
constexpr size_t ANCHOR_SIZE = 50;
constexpr size_t RECORD_SIZE = LABEL_SIZE + sizeof(uint16_t) + ANCHOR_SIZE;
constexpr size_t HEADER_SIZE = 4;
// Far beyond any real book; bounds the file a hostile EPUB can make us write.
constexpr uint16_t MAX_ENTRIES = 8000;

struct Entry {
  char label[LABEL_SIZE];
  uint16_t spineIndex;
  char anchor[ANCHOR_SIZE];
};

std::string filePath(const std::string& cachePath);

// Streams entries to the sidecar as the TOC parser meets them.
class Writer {
 public:
  // Removes any previous file. A failure to open only disables add().
  void begin(const std::string& cachePath);
  // Entries that cannot be represented (no label, unknown spine item, an
  // anchor longer than the record) are skipped.
  void add(const std::string& label, int spineIndex, const std::string& anchor);
  // Patches the count into the header, or removes the file when nothing was
  // written. Returns the number of entries.
  uint16_t finish();

  // Scratch for the caller's href -> spine lookup (see
  // BookMetadataCache::createPageListEntry); freed by finish().
  std::string memoHref;
  int memoSpine = -1;
  bool memoValid = false;

 private:
  FsFile file;
  std::string path;
  uint16_t count = 0;
  bool open = false;
};

// Number of pages listed for the book; 0 when it has no page list.
uint16_t count(const std::string& cachePath);

// Reads one entry. False on a missing file or an index out of range.
bool read(const std::string& cachePath, uint16_t index, Entry& out);

// Index of the first entry whose spine item is >= spineIndex (== total when
// there is none). Binary search, ~log2(total) record reads.
uint16_t lowerBoundSpine(const std::string& cachePath, uint16_t total, uint16_t spineIndex);

}  // namespace PageList
