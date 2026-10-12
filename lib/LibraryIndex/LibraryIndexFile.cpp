#include "LibraryIndexFile.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "LibraryText.h"

namespace library {

LibraryIndexFile::~LibraryIndexFile() { close(); }

bool LibraryIndexFile::open(const char* path) { return openImpl(path, false); }

bool LibraryIndexFile::openForReconciliation(const char* path) { return openImpl(path, true); }

bool LibraryIndexFile::openImpl(const char* path, const bool acceptStaleFold) {
  close();
  readFailed = false;
  if (!Storage.openFileForRead("LIBIDX", path, file)) {
    readFailed = true;
    return false;
  }

  if (file.read(&head, sizeof(head)) != static_cast<int>(sizeof(head))) {
    readFailed = true;
    lastValidity = ClixValidity::SizeMismatch;
    file.close();
    return false;
  }

  // Older layouts share the header and records. Reconciliation can recover
  // their path hashes and arrival history before writing the current layout.
  lastValidity = acceptStaleFold ? validateHeaderStructure(head, file.fileSize64(), true)
                                 : validateHeader(head, file.fileSize64());
  if (lastValidity != ClixValidity::Ok) {
    LOG_INF("LIBIDX", "index rejected: %s", clixValidityName(lastValidity));
    file.close();
    return false;
  }
  opened = true;
  return true;
}

void LibraryIndexFile::close() {
  if (file.isOpen()) file.close();
  opened = false;
}

bool LibraryIndexFile::readAt(const uint32_t offset, void* dst, const size_t len) {
  if (!opened) return false;
  // Every offset handed to this function comes from the header, and the header
  // was validated against the real file size, so a short read means the card
  // changed under us rather than a bad computation.
  if (!file.seekSet(offset) || file.read(dst, len) != static_cast<int>(len)) {
    readFailed = true;
    return false;
  }
  return true;
}

uint16_t LibraryIndexFile::ordinalForRow(const SortOrder order, const uint16_t row) {
  constexpr uint16_t NONE = 0xFFFF;
  if (!opened || row >= head.bookCount) return NONE;

  switch (order) {
    case SortOrder::TitleAsc:
      // The record section IS in title order, so this costs no storage and no
      // read at all.
      return row;
    case SortOrder::TitleDesc:
      return static_cast<uint16_t>(head.bookCount - 1 - row);
    case SortOrder::AuthorAsc:
    case SortOrder::AuthorDesc: {
      const uint16_t k = order == SortOrder::AuthorAsc ? row : static_cast<uint16_t>(head.bookCount - 1 - row);
      uint16_t ordinal = NONE;
      return readAt(authorOrderOffset(head, k), &ordinal, sizeof(ordinal)) && ordinal < head.bookCount ? ordinal : NONE;
    }
    case SortOrder::AuthorFirstAsc:
    case SortOrder::AuthorFirstDesc: {
      const uint16_t k = order == SortOrder::AuthorFirstAsc ? row : static_cast<uint16_t>(head.bookCount - 1 - row);
      uint16_t ordinal = NONE;
      return readAt(firstNameOrderOffset(head, k), &ordinal, sizeof(ordinal)) && ordinal < head.bookCount ? ordinal
                                                                                                          : NONE;
    }
    case SortOrder::RecentAsc:
    case SortOrder::RecentDesc: {
      // arrivalOrder runs oldest first, so both directions share one on-disk
      // permutation.
      const uint16_t k = order == SortOrder::RecentAsc ? row : static_cast<uint16_t>(head.bookCount - 1 - row);
      uint16_t ordinal = NONE;
      return readAt(arrivalOrderOffset(head, k), &ordinal, sizeof(ordinal)) && ordinal < head.bookCount ? ordinal
                                                                                                        : NONE;
    }
    case SortOrder::SeriesAsc:
    case SortOrder::SeriesDesc:
    case SortOrder::GenreAsc:
    case SortOrder::GenreDesc: {
      if (head.formatVersion < 4) return NONE;
      const bool series = order == SortOrder::SeriesAsc || order == SortOrder::SeriesDesc;
      const bool ascending = order == SortOrder::SeriesAsc || order == SortOrder::GenreAsc;
      const uint16_t k = ascending ? row : static_cast<uint16_t>(head.bookCount - 1 - row);
      uint16_t ordinal = NONE;
      const uint32_t offset = series ? seriesOrderOffset(head, k) : genreOrderOffset(head, k);
      return readAt(offset, &ordinal, sizeof(ordinal)) && ordinal < head.bookCount ? ordinal : NONE;
    }
  }
  return NONE;
}

bool LibraryIndexFile::readOrder(const SortOrder order, uint16_t* out) {
  if (!opened || !out) return false;
  const uint16_t n = head.bookCount;
  if (n == 0) return true;

  uint32_t offset = 0;
  switch (order) {
    case SortOrder::TitleAsc:
    case SortOrder::TitleDesc:
      // The record section IS in title order: no storage, no read.
      for (uint16_t i = 0; i < n; i++) out[i] = i;
      return true;
    case SortOrder::AuthorAsc:
    case SortOrder::AuthorDesc:
      offset = authorOrderOffset(head, 0);
      break;
    case SortOrder::AuthorFirstAsc:
    case SortOrder::AuthorFirstDesc:
      offset = firstNameOrderOffset(head, 0);
      break;
    case SortOrder::RecentAsc:
    case SortOrder::RecentDesc:
      offset = arrivalOrderOffset(head, 0);
      break;
    case SortOrder::SeriesAsc:
    case SortOrder::SeriesDesc:
      if (head.formatVersion < 4) return false;
      offset = seriesOrderOffset(head, 0);
      break;
    case SortOrder::GenreAsc:
    case SortOrder::GenreDesc:
      if (head.formatVersion < 4) return false;
      offset = genreOrderOffset(head, 0);
      break;
  }
  if (!readAt(offset, out, static_cast<size_t>(n) * sizeof(uint16_t))) return false;
  // The entries come off a card the user can write to; an out-of-range ordinal
  // must not reach readRecord() callers as if it were a row.
  for (uint16_t i = 0; i < n; i++) {
    if (out[i] >= n) {
      LOG_ERR("LIBIDX", "order entry %u out of range", static_cast<unsigned>(i));
      return false;
    }
  }
  return true;
}

bool LibraryIndexFile::forEachPathHash(const PathHashVisitor visit, void* const context) {
  if (!opened || !visit) return false;
  if (head.bookCount == 0) return true;

  // 32 records fill 4096 bytes exactly (the aligned-tile size), so the record
  // section streams in whole sectors. Heap, not stack: far over the 256-byte
  // local budget. Freed when this function returns.
  constexpr size_t CHUNK_RECORDS = 32;
  auto chunk = makeUniqueNoThrow<uint8_t[]>(CHUNK_RECORDS * sizeof(ClixRecord));
  if (!chunk) {
    LOG_ERR("LIBIDX", "OOM: %u-byte record chunk", static_cast<unsigned>(CHUNK_RECORDS * sizeof(ClixRecord)));
    return false;
  }
  for (uint32_t base = 0; base < head.bookCount; base += CHUNK_RECORDS) {
    const uint16_t batch = static_cast<uint16_t>(std::min<uint32_t>(CHUNK_RECORDS, head.bookCount - base));
    if (!readAt(recordOffset(head, static_cast<uint16_t>(base)), chunk.get(), batch * sizeof(ClixRecord))) return false;
    for (uint16_t r = 0; r < batch; r++) {
      // memcpy, not a cast: the chunk buffer has no alignment guarantee for the
      // record's 32-bit fields.
      ClixRecord record;
      memcpy(&record, chunk.get() + r * sizeof(ClixRecord), sizeof(ClixRecord));
      uint64_t hash = 0;
      if (!readPathHash(record, hash)) return false;
      if (!visit(context, static_cast<uint16_t>(base + r), hash)) return true;
    }
  }
  return true;
}

bool LibraryIndexFile::readRecord(const uint16_t ordinal, ClixRecord& out) {
  if (!opened || ordinal >= head.bookCount) return false;
  if (!readAt(recordOffset(head, ordinal), &out, sizeof(out))) return false;

  // Clamp here, at the single point every record enters the program. These
  // lengths come off an SD card that the user can write to and that can rot: a
  // foldLen of 255 against a 96-byte field sends a string_view 159 bytes past the
  // end of the record, and callers build views from them without looking. Fixing
  // it at each call site would mean fixing it again at the next one.
  out.foldLen = static_cast<uint8_t>(std::min<size_t>(out.foldLen, CLIX_FOLD_BYTES));
  out.authorKeyLen = static_cast<uint8_t>(std::min<size_t>(out.authorKeyLen, CLIX_AUTHOR_KEY_BYTES));
  if (out.metadataStatus > CLIX_METADATA_FAILED) return false;
  // nameOff is u32 and every reader adds a length to it before comparing against
  // the section size. A forged value near the top of the range wraps that sum and
  // passes the bounds check it was supposed to fail, so it is rejected here
  // instead — the one place that can, before any arithmetic touches it.
  if (out.nameOff > head.nameLen) {
    out.nameLen = 0;
    out.nameOff = 0;
  }
  return true;
}

bool LibraryIndexFile::readCreationTime(const uint16_t ordinal, uint32_t& out) {
  out = 0;
  if (!opened || head.formatVersion < 5 || ordinal >= head.bookCount) return false;
  return readAt(creationTimeOffset(head, ordinal), &out, sizeof(out));
}

bool LibraryIndexFile::readName(const ClixRecord& record, std::string& out) {
  out.clear();
  if (!opened || record.nameLen == 0) return false;
  if (record.nameOff > head.nameLen || sizeof(uint64_t) > head.nameLen - record.nameOff ||
      record.nameLen > head.nameLen - record.nameOff - sizeof(uint64_t))
    return false;
  out.resize(record.nameLen);
  return readAt(head.nameStart + record.nameOff + sizeof(uint64_t), out.data(), record.nameLen);
}

bool LibraryIndexFile::readPathHash(const ClixRecord& record, uint64_t& out) {
  out = 0;
  if (!opened) return false;
  if (record.nameOff > head.nameLen || sizeof(out) > head.nameLen - record.nameOff) {
    readFailed = true;
    return false;
  }
  return readAt(head.nameStart + record.nameOff, &out, sizeof(out));
}

bool LibraryIndexFile::readBlobField(const ClixRecord& record, const uint8_t field, std::string& out) {
  out.clear();
  if (!opened || record.nameLen == 0) return false;
  if (record.nameOff > head.nameLen || sizeof(uint64_t) > head.nameLen - record.nameOff ||
      record.nameLen > head.nameLen - record.nameOff - sizeof(uint64_t))
    return false;

  uint32_t at = record.nameOff + sizeof(uint64_t) + record.nameLen;
  for (uint8_t i = 0; i <= field; i++) {
    if (at >= head.nameLen) return false;
    uint8_t len = 0;
    if (!readAt(head.nameStart + at, &len, sizeof(len))) return false;
    ++at;
    if (len > head.nameLen - at) return false;
    if (i == field) {
      out.resize(len);
      return len == 0 || readAt(head.nameStart + at, out.data(), len);
    }
    at += len;
  }
  return false;
}

bool LibraryIndexFile::readAuthor(const ClixRecord& record, std::string& out) {
  return readBlobField(record, 0, out) && !out.empty();
}

// The book's own title, after the name and the author. Absent (length 0) for a
// book that never told us one, in which case the caller shows the filename.
bool LibraryIndexFile::readTitle(const ClixRecord& record, std::string& out) {
  return readBlobField(record, 1, out) && !out.empty();
}

bool LibraryIndexFile::readDisplayText(const ClixRecord& record, std::string& title, std::string& author) {
  if (!readBlobField(record, 1, title) || !readBlobField(record, 0, author)) return false;
  if (title.empty()) {
    if (!readName(record, title)) return false;
    const size_t dot = title.find_last_of('.');
    if (dot != std::string::npos && dot != 0) title.resize(dot);
  }
  return true;
}

bool LibraryIndexFile::readSourceAuthor(const ClixRecord& record, std::string& out) {
  return readBlobField(record, 2, out);
}

bool LibraryIndexFile::readSeries(const ClixRecord& record, std::string& out) {
  return head.formatVersion >= 3 && readBlobField(record, 3, out);
}

bool LibraryIndexFile::readGenre(const ClixRecord& record, std::string& out) {
  return head.formatVersion >= 3 && readBlobField(record, 4, out);
}

bool LibraryIndexFile::readSeriesPosition(const ClixRecord& record, uint32_t& out) {
  out = CLIX_UNKNOWN_SERIES_POSITION;
  if (!opened || head.formatVersion < 6 || record.nameOff > head.nameLen ||
      sizeof(uint64_t) + record.nameLen > head.nameLen - record.nameOff)
    return false;
  uint32_t at = record.nameOff + sizeof(uint64_t) + record.nameLen;
  for (uint8_t field = 0; field < 5; field++) {
    if (at >= head.nameLen) return false;
    uint8_t length = 0;
    if (!readAt(head.nameStart + at, &length, sizeof(length))) return false;
    ++at;
    if (length > head.nameLen - at) return false;
    at += length;
  }
  return at <= head.nameLen && sizeof(out) <= head.nameLen - at && readAt(head.nameStart + at, &out, sizeof(out));
}

bool LibraryIndexFile::readPath(const ClixRecord& record, std::string& out) {
  return readPath(record, out, nullptr, 0);
}

bool LibraryIndexFile::buildFolderCheckpoints(uint32_t* offsets, uint16_t& stride) {
  stride = 0;
  if (!opened || !offsets || head.folderCount == 0) return false;
  stride = (head.folderCount + FOLDER_CHECKPOINT_COUNT - 1) / FOLDER_CHECKPOINT_COUNT;
  uint32_t offset = head.folderStart;
  const uint32_t folderEnd = head.folderStart + head.folderLen;
  for (uint32_t i = 0; i < head.folderCount; ++i) {
    if (offset >= folderEnd) return false;
    if (i % stride == 0) offsets[i / stride] = offset;
    uint8_t pathLen = 0;
    if (!readAt(offset, &pathLen, sizeof(pathLen)) || pathLen == 0 || pathLen > folderEnd - offset - 1u) return false;
    offset += 1u + pathLen;
  }
  return true;
}

bool LibraryIndexFile::readPath(const ClixRecord& record, std::string& out, const uint32_t* offsets,
                                const uint16_t stride) {
  out.clear();
  if (!opened || record.folderId >= head.folderCount) return false;

  // Folder records are variable length. A scan can supply sparse offsets to
  // bound each walk; ordinary single-book reads still start at folder zero.
  const uint16_t firstFolder = offsets && stride ? (record.folderId / stride) * stride : 0;
  uint32_t offset = offsets && stride ? offsets[record.folderId / stride] : head.folderStart;
  const uint32_t folderEnd = head.folderStart + head.folderLen;
  for (uint32_t i = firstFolder; i <= record.folderId; i++) {
    if (offset >= folderEnd) return false;
    uint8_t pathLen = 0;
    if (!readAt(offset, &pathLen, sizeof(pathLen)) || pathLen == 0) return false;
    if (pathLen > folderEnd - offset - 1u) return false;
    if (i == record.folderId) {
      std::string dir(pathLen, '\0');
      if (!readAt(offset + 1, dir.data(), pathLen)) return false;
      std::string name;
      if (!readName(record, name)) return false;
      out = joinLibraryPath(dir, name);
      return true;
    }
    offset += 1u + pathLen;
    if (offset >= folderEnd) return false;
  }
  return false;
}

}  // namespace library
