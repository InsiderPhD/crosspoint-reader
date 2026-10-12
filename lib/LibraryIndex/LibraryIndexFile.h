#pragma once

// Read side of the CLX1 index.
//
// Holds one open file handle and a few hundred bytes; nothing that scales with
// the library stays resident. A page of rows is a handful of seeks, which is
// what the fixed 128-byte record stride buys — record k is always at
// recordStart + 128k, so no offset table has to be loaded to find it.

#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "LibraryFormat.h"

namespace library {

enum class SortOrder : uint8_t {
  // "Recent" orders by file creation time, oldest first in Asc; firstSeen
  // breaks ties and orders files without creation timestamps.
  RecentAsc,
  RecentDesc,
  TitleAsc,
  TitleDesc,
  AuthorAsc,
  AuthorDesc,
  AuthorFirstAsc,
  AuthorFirstDesc,
  SeriesAsc,
  SeriesDesc,
  GenreAsc,
  GenreDesc,
};

// Receives every book's record ordinal and complete-path hash (clixPathHash) in
// record order. Return false to stop the scan early (not an error).
using PathHashVisitor = bool (*)(void* context, uint16_t ordinal, uint64_t pathHash);

class LibraryIndexFile {
 public:
  LibraryIndexFile() = default;
  ~LibraryIndexFile();
  LibraryIndexFile(const LibraryIndexFile&) = delete;
  LibraryIndexFile& operator=(const LibraryIndexFile&) = delete;

  // Open and validate. On failure `validity()` says why, so a rebuild loop
  // caused by a format bug is visible in the log rather than looking like a slow
  // first boot.
  bool open(const char* path);
  // Accept an otherwise valid stale fold so a rebuild can preserve arrival
  // history without exposing stale sort/search keys to the browser.
  bool openForReconciliation(const char* path);
  void close();
  bool isOpen() const { return opened; }
  bool ioFailed() const { return readFailed; }

  ClixValidity validity() const { return lastValidity; }
  const ClixHeader& header() const { return head; }
  uint16_t bookCount() const { return opened ? head.bookCount : 0; }
  bool ranksDegraded() const { return opened && (head.flags & CLIX_FLAG_RANKS_DEGRADED) != 0; }
  bool dedupDegraded() const { return opened && (head.flags & CLIX_FLAG_DEDUP_DEGRADED) != 0; }

  // Record ordinal of the row at display position `row` in `order`. Returns
  // 0xFFFF when out of range, which callers treat as "no such row" rather than
  // indexing anyway.
  uint16_t ordinalForRow(SortOrder order, uint16_t row);

  // Fork additions (CrossInk has recentRowsFor() here instead, which this
  // fork's Library does not need).
  //
  // The whole ascending permutation for `order`, bookCount() ordinals into
  // `out`, in one read: what a caller-owned row view is built from. Descending
  // orders are the caller's reversal. False on I/O failure or a corrupt entry.
  bool readOrder(SortOrder order, uint16_t* out);
  // One pass over every book's path hash: how reading history, finished books
  // and BookFusion links (all keyed by path) are located in the index without
  // rebuilding a single path. Transient 4 KiB chunk buffer, freed on return.
  bool forEachPathHash(PathHashVisitor visit, void* context);

  bool readRecord(uint16_t ordinal, ClixRecord& out);
  // V5 creation timestamp in the title-ordered side array. Zero means the
  // filesystem provided no creation time. Older versions have no such array.
  bool readCreationTime(uint16_t ordinal, uint32_t& out);
  // Persisted complete-path fingerprint used by rebuild reconciliation.
  bool readPathHash(const ClixRecord& record, uint64_t& out);

  // Display basename, exactly as it sits on the card. This is the only string
  // the UI draws, and it is never shortened on disk.
  bool readName(const ClixRecord& record, std::string& out);
  // The author the build settled on, stored right after the name. Reading it
  // rather than re-deriving it from the name is what makes the metadata pass and
  // the spelling harmonisation visible: neither survives a filename that no
  // longer carries "Title - Author".
  bool readAuthor(const ClixRecord& record, std::string& out);
  bool readTitle(const ClixRecord& record, std::string& out);
  // Checked display text for browsing/search. Empty metadata is valid: use
  // the filename stem for a missing title and leave a missing author blank.
  bool readDisplayText(const ClixRecord& record, std::string& title, std::string& author);
  // Cleaned author spelling before the library-wide spelling vote. Empty is a
  // valid value, so success is independent of `out.empty()`.
  bool readSourceAuthor(const ClixRecord& record, std::string& out);
  bool readSeries(const ClixRecord& record, std::string& out);
  bool readGenre(const ClixRecord& record, std::string& out);
  // V6 sortable signed float bits; UINT32_MAX means no usable order.
  bool readSeriesPosition(const ClixRecord& record, uint32_t& out);

  // Absolute path of the book, rebuilt from its folder record.
  bool readPath(const ClixRecord& record, std::string& out);
  // A small, caller-owned offset table bounds folder walks during full-library scans.
  static constexpr uint16_t FOLDER_CHECKPOINT_COUNT = 128;
  bool buildFolderCheckpoints(uint32_t* offsets, uint16_t& stride);
  bool readPath(const ClixRecord& record, std::string& out, const uint32_t* offsets, uint16_t stride);

 private:
  bool openImpl(const char* path, bool acceptStaleFold);
  bool readAt(uint32_t offset, void* dst, size_t len);
  bool readBlobField(const ClixRecord& record, uint8_t field, std::string& out);

  HalFile file;
  ClixHeader head{};
  bool opened = false;
  bool readFailed = false;
  ClixValidity lastValidity = ClixValidity::BadMagic;
};

}  // namespace library
