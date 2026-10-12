#pragma once

// Persistent EPUB metadata, written as each book is parsed.
//
// The Library index already reuses metadata from the previous index, but only
// once a whole build has finished and installed. A huge card can take far
// longer than anyone waits, and a cancelled or failed build used to throw every
// parse away. This cache keeps each parse the moment it happens, so the next
// build resumes where the last one stopped without remembering where that was:
// a book is found again by its identity, never by a directory cursor, so files
// added, moved, or removed in between do not matter.
//
// Two files under /.crosspoint:
//
//   library.clm   header, then a fixed open-addressing table of 16-byte slots
//                 {pathHash, payloadOffset, check}. 65,536 slots is twice the
//                 format ceiling, so probes stay short; the table is 1 MiB.
//   library.cld   append-only payload records {pathHash, size, mtime, series
//                 position, string lengths, checksum, strings}.
//
// The identity is the complete-path hash plus the file size and modification
// time; any change to the book is a miss. Every slot and payload carries a
// checksum, and anything that does not verify is treated as absent, so a torn
// write after a power cut costs a re-parse, never wrong metadata.
//
// Nothing here scales with the library in RAM: one open handle per file and a
// few counters. Lookups are a couple of short seeks against a parse that takes
// hundreds of milliseconds.

#include <HalStorage.h>

#include <cstdint>
#include <string>

namespace library {

struct CachedBookMetadata {
  std::string title;
  std::string author;
  std::string series;
  std::string genre;
  uint32_t seriesPosition = 0;
};

class LibraryMetadataCache {
 public:
  static constexpr uint32_t SLOT_COUNT = 65536;
  // Reset at open time once the table is this full or the payload this large;
  // both only grow as books are renamed or edited over the card's life.
  static constexpr uint32_t MAX_USED_SLOTS = SLOT_COUNT / 4 * 3;
  static constexpr uint32_t MAX_PAYLOAD_BYTES = 64u * 1024u * 1024u;
  static constexpr uint32_t MAX_PROBES = 64;
  // Each field is stored cut at a UTF-8 boundary to this length. The index
  // keeps no more of a title, and an author longer than this has already lost
  // its tail to the index's 128-byte display field, so a resumed build only
  // differs from a fresh parse for such outsized values.
  static constexpr size_t MAX_FIELD_BYTES = 255;

  LibraryMetadataCache() = default;
  ~LibraryMetadataCache();
  LibraryMetadataCache(const LibraryMetadataCache&) = delete;
  LibraryMetadataCache& operator=(const LibraryMetadataCache&) = delete;

  // Opens an existing cache. A missing one is created lazily by the first
  // store(), so libraries that never parse anything never pay for the table.
  // Creating it writes 1 MiB; `service` runs between chunks and `stopped`
  // abandons the write, as for the rest of the build. Both may be null.
  void open(void (*service)() = nullptr, bool (*stopped)() = nullptr);
  // Flushes the slot count. Safe to call more than once.
  void close();

  // False on any miss, mismatch, or unreadable record.
  bool lookup(uint64_t pathHash, uint32_t fileSize, uint32_t modificationTime, CachedBookMetadata& out);
  // Best effort: a failure is logged and the cache disables itself for the
  // rest of the build rather than failing the build.
  void store(uint64_t pathHash, uint32_t fileSize, uint32_t modificationTime, const CachedBookMetadata& metadata);

  static const char* slotPath();
  static const char* payloadPath();

 private:
  bool create();
  bool openExisting();
  void disable();
  void discard();
  bool readSlot(uint32_t index, uint64_t& pathHash, uint32_t& payloadOffset, bool& empty);
  bool writeSlot(uint32_t index, uint64_t pathHash, uint32_t payloadOffset);

  HalFile slots;
  HalFile payload;
  void (*service)() = nullptr;
  bool (*stopped)() = nullptr;
  uint32_t usedSlots = 0;
  uint32_t payloadEnd = 0;
  uint16_t unsyncedStores = 0;
  bool opened = false;
  bool disabled = false;
  bool headerDirty = false;
};

}  // namespace library
