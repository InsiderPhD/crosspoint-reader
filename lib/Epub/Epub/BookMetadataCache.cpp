#include "BookMetadataCache.h"

#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <cstring>
#include <deque>
#include <new>

#include "FsHelpers.h"

namespace {
// v12: multiple <dc:creator> authors are now joined with " & " (was ", ") so the Authors
// folder view can split co-authors without breaking "Last, First" names. Bumping forces a
// rebuild of stale book.bin so the fix applies without a manual recache.
// v13: the publisher page list is written to pagelist.bin during the TOC pass (PageList.h).
// book.bin itself is unchanged; the bump only makes existing books rebuild so they gain it.
constexpr uint8_t BOOK_CACHE_VERSION = 13;
constexpr char bookBinFile[] = "/book.bin";
// Cap on the in-RAM cumulative spine size table: 1024 x 4 bytes = 4KB, held for
// the reading session. Books with more spine items keep the on-disk lookup.
constexpr uint16_t MAX_CACHED_CUMULATIVE_SIZES = 1024;
// Transient read buffer used only while filling that table (heap: over the
// 256-byte stack-local budget; freed before the fill returns).
constexpr size_t CUMULATIVE_SIZE_READ_CHUNK = 1024;
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";

// HalFile::read() returns a negative int on error; never compare it to a size_t directly.
bool readExact(FsFile& file, void* out, const size_t count) {
  const int got = file.read(out, count);
  return got >= 0 && static_cast<size_t>(got) == count;
}
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  return Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile);
}

bool BookMetadataCache::endContentOpfPass() {
  // Explicit close() required: member variable persists beyond function scope
  spineFile.close();
  return true;
}

bool BookMetadataCache::beginTocPass() {
  LOG_DBG("BMC", "Beginning toc pass");

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variable persists beyond function scope
    spineFile.close();
    return false;
  }

  // Optional: a book whose page list cannot be written simply has no Go to
  // Page. One small transient object, freed in endTocPass().
  pageListWriter.reset(new (std::nothrow) PageList::Writer());
  if (pageListWriter) pageListWriter->begin(cachePath);

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex.clear();
    spineHrefIndex.resize(spineCount);
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      SpineHrefIndexEntry idx;
      idx.hrefHash = fnvHash64(entry.href);
      idx.hrefLen = static_cast<uint16_t>(entry.href.size());
      idx.spineIndex = static_cast<int16_t>(i);
      spineHrefIndex[i] = idx;
    }
    std::sort(spineHrefIndex.begin(), spineHrefIndex.end(),
              [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
              });
    spineFile.seek(0);
    useSpineHrefIndex = true;
    LOG_DBG("BMC", "Using fast index for %d spine items", spineCount);
  } else {
    useSpineHrefIndex = false;
  }

  return true;
}

bool BookMetadataCache::endTocPass() {
  // Explicit close() required: member variables persist beyond function scope
  tocFile.close();
  spineFile.close();

  if (pageListWriter) {
    pageListWriter->finish();
    pageListWriter.reset();
  }

  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;

  return true;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  LOG_DBG("BMC", "Wrote %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  // Open all three files, writing to meta, reading from spine and toc
  if (!Storage.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variables persist beyond function scope
    bookFile.close();
    spineFile.close();
    return false;
  }

  constexpr uint32_t headerASize =
      sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                metadata.tags.size() + metadata.seriesName.size() + metadata.seriesIndex.size() +
                                metadata.publisher.size() + metadata.pubDate.size() + metadata.rating.size() +
                                metadata.bookshelf.size() + sizeof(uint32_t) * 12;
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Header A
  serialization::writePod(bookFile, BOOK_CACHE_VERSION);
  serialization::writePod(bookFile, lutOffset);
  serialization::writePod(bookFile, spineCount);
  serialization::writePod(bookFile, tocCount);
  // Metadata
  serialization::writeString(bookFile, metadata.title);
  serialization::writeString(bookFile, metadata.author);
  serialization::writeString(bookFile, metadata.language);
  serialization::writeString(bookFile, metadata.coverItemHref);
  serialization::writeString(bookFile, metadata.textReferenceHref);
  serialization::writeString(bookFile, metadata.tags);
  serialization::writeString(bookFile, metadata.seriesName);
  serialization::writeString(bookFile, metadata.seriesIndex);
  serialization::writeString(bookFile, metadata.publisher);
  serialization::writeString(bookFile, metadata.pubDate);
  serialization::writeString(bookFile, metadata.rating);
  serialization::writeString(bookFile, metadata.bookshelf);

  // Loop through spine entries, writing LUT positions
  spineFile.seek(0);
  for (int i = 0; i < spineCount; i++) {
    uint32_t pos = spineFile.position();
    auto spineEntry = readSpineEntry(spineFile);
    serialization::writePod(bookFile, pos + lutOffset + lutSize);
  }

  // Loop through toc entries, writing LUT positions
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    uint32_t pos = tocFile.position();
    auto tocEntry = readTocEntry(tocFile);
    serialization::writePod(bookFile, pos + lutOffset + lutSize + static_cast<uint32_t>(spineFile.position()));
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m))
  std::deque<int16_t> spineToTocIndex(spineCount, -1);
  tocFile.seek(0);
  for (int j = 0; j < tocCount; j++) {
    auto tocEntry = readTocEntry(tocFile);
    if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < spineCount) {
      if (spineToTocIndex[tocEntry.spineIndex] == -1) {
        spineToTocIndex[tocEntry.spineIndex] = static_cast<int16_t>(j);
      }
    }
  }

  ZipFile zip(epubPath);
  // Pre-open zip file to speed up size calculations
  if (!zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    // Explicit close() required: member variables persist beyond function scope
    bookFile.close();
    spineFile.close();
    tocFile.close();
    return false;
  }
  // NOTE: We intentionally skip calling loadAllFileStatSlims() here.
  // For large EPUBs (2000+ chapters), pre-loading all ZIP central directory entries
  // into memory causes OOM crashes on ESP32-C3's limited ~380KB RAM.
  // Instead, for large books we use a batch lookup that scans the ZIP central
  // directory once per chunk and matches against spine targets using hash comparison.
  // This is O(n*log(m)) instead of O(n*m) while avoiding memory exhaustion.
  // See: https://github.com/crosspoint-reader/crosspoint-reader/issues/134

  std::deque<uint32_t> spineSizes;
  bool useBatchSizes = false;

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    LOG_DBG("BMC", "Using batch size lookup for %d spine items", spineCount);

    // Targets cost 16 bytes per spine item. Building them for the whole spine at
    // once is 80KB for a 5000-chapter book, so look sizes up in bounded chunks
    // (512 targets = ~8KB) at the price of one central-directory scan per chunk.
    constexpr size_t SIZE_TARGET_CHUNK = 512;
    std::deque<ZipFile::SizeTarget> targets;
    spineSizes.resize(spineCount, 0);
    int matched = 0;

    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      std::string path = FsHelpers::normalisePath(entry.href);

      ZipFile::SizeTarget t;
      t.hash = ZipFile::fnvHash64(path.c_str(), path.size());
      t.len = static_cast<uint16_t>(path.size());
      t.index = static_cast<uint16_t>(i);
      targets.push_back(t);
      if (targets.size() < SIZE_TARGET_CHUNK && i + 1 < spineCount) continue;

      std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });
      matched += zip.fillUncompressedSizes(targets, spineSizes);
      targets.clear();
    }
    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);

    targets.shrink_to_fit();

    useBatchSizes = true;
  }

  uint32_t cumSize = 0;
  spineFile.seek(0);
  int lastSpineTocIndex = -1;
  for (int i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntry(spineFile);

    spineEntry.tocIndex = spineToTocIndex[i];

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (spineEntry.tocIndex == -1) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
              spineEntry.href.c_str());
      spineEntry.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineEntry.tocIndex;

    size_t itemSize = 0;
    if (useBatchSizes) {
      itemSize = spineSizes[i];
      if (itemSize == 0) {
        const std::string path = FsHelpers::normalisePath(spineEntry.href);
        if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
          LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
        }
      }
    } else {
      const std::string path = FsHelpers::normalisePath(spineEntry.href);
      if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
        LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
      }
    }

    cumSize += itemSize;
    spineEntry.cumulativeSize = cumSize;

    // Write out spine data to book.bin
    writeSpineEntry(bookFile, spineEntry);
  }
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    auto tocEntry = readTocEntry(tocFile);
    writeTocEntry(bookFile, tocEntry);
  }

  // Explicit close() required: member variables persist beyond function scope
  bookFile.close();
  spineFile.close();
  tocFile.close();

  LOG_DBG("BMC", "Successfully built book.bin");
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    Storage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    Storage.remove(tocBinFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(FsFile& file, const SpineEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
  return pos;
}

uint32_t BookMetadataCache::writeTocEntry(FsFile& file, const TocEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntry(spineFile, entry);
  spineCount++;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  const int16_t spineIndex = findSpineIndexForHref(href);
  if (spineIndex == -1) {
    LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
  }

  const TocEntry entry(title, href, anchor, level, spineIndex);
  writeTocEntry(tocFile, entry);
  tocCount++;
}

void BookMetadataCache::createPageListEntry(const std::string& label, const std::string& href,
                                            const std::string& anchor) {
  if (!buildMode || !spineFile || !pageListWriter) return;
  // A page list has hundreds of entries but they run in document order, so
  // consecutive ones share a file: resolve each file once, not each page. (The
  // small-spine lookup is a linear read of the spine temp file.)
  if (!pageListWriter->memoValid || pageListWriter->memoHref != href) {
    pageListWriter->memoHref = href;
    pageListWriter->memoSpine = findSpineIndexForHref(href);
    pageListWriter->memoValid = true;
  }
  // An entry pointing outside the spine has nowhere to jump to; the writer drops it.
  pageListWriter->add(label, pageListWriter->memoSpine, anchor);
}

int16_t BookMetadataCache::findSpineIndexForHref(const std::string& href) {
  int16_t spineIndex = -1;

  if (useSpineHrefIndex) {
    uint64_t targetHash = fnvHash64(href);
    uint16_t targetLen = static_cast<uint16_t>(href.size());

    auto it =
        std::lower_bound(spineHrefIndex.begin(), spineHrefIndex.end(), SpineHrefIndexEntry{targetHash, targetLen, 0},
                         [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                           return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                         });

    while (it != spineHrefIndex.end() && it->hrefHash == targetHash && it->hrefLen == targetLen) {
      spineIndex = it->spineIndex;
      break;
    }

  } else {
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto spineEntry = readSpineEntry(spineFile);
      if (spineEntry.href == href) {
        spineIndex = static_cast<int16_t>(i);
        break;
      }
    }
  }
  return spineIndex;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::isCurrent(const std::string& cachePath) {
  const std::string path = cachePath + bookBinFile;
  // exists() first: openFileForRead logs an error for a missing file, and a
  // missing book.bin is the normal first-open case, not a fault.
  if (!Storage.exists(path.c_str())) return false;
  FsFile file;
  if (!Storage.openFileForRead("BMC", path, file)) return false;
  uint8_t version = 0;
  const bool ok = file.read(&version, 1) == 1 && version == BOOK_CACHE_VERSION;
  file.close();
  return ok;
}

bool BookMetadataCache::load() {
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  cumulativeSizes.reset();
  cumulativeSizeCount = 0;
  cumulativeSizesTried = false;

  uint8_t version;
  serialization::readPod(bookFile, version);
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  serialization::readPod(bookFile, lutOffset);
  serialization::readPod(bookFile, spineCount);
  serialization::readPod(bookFile, tocCount);

  serialization::readString(bookFile, coreMetadata.title);
  serialization::readString(bookFile, coreMetadata.author);
  serialization::readString(bookFile, coreMetadata.language);
  serialization::readString(bookFile, coreMetadata.coverItemHref);
  serialization::readString(bookFile, coreMetadata.textReferenceHref);
  serialization::readString(bookFile, coreMetadata.tags);
  serialization::readString(bookFile, coreMetadata.seriesName);
  serialization::readString(bookFile, coreMetadata.seriesIndex);
  serialization::readString(bookFile, coreMetadata.publisher);
  serialization::readString(bookFile, coreMetadata.pubDate);
  serialization::readString(bookFile, coreMetadata.rating);
  serialization::readString(bookFile, coreMetadata.bookshelf);

  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
    return {};
  }

  // Seek to spine LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * index);
  uint32_t spineEntryPos;
  serialization::readPod(bookFile, spineEntryPos);
  bookFile.seek(spineEntryPos);
  return readSpineEntry(bookFile);
}

void BookMetadataCache::cacheSpineCumulativeSizes() {
  // One attempt per load(): a failed allocation or a short read must not turn
  // every later lookup into another failed attempt.
  cumulativeSizesTried = true;
  cumulativeSizeCount = 0;
  if (spineCount == 0 || spineCount > MAX_CACHED_CUMULATIVE_SIZES) {
    return;
  }

  auto sizes = makeUniqueNoThrow<uint32_t[]>(spineCount);
  auto chunk = makeUniqueNoThrow<uint8_t[]>(CUMULATIVE_SIZE_READ_CHUNK);
  if (!sizes || !chunk) {
    LOG_DBG("BMC", "Could not allocate %u-byte cumulative size cache; using SD lookups",
            static_cast<unsigned>(spineCount * sizeof(uint32_t)));
    return;
  }

  // Spine entries are written back to back in spine order (buildBookBin), so one
  // forward pass from the first entry covers them all.
  uint32_t firstEntryPos = 0;
  if (!bookFile.seek(lutOffset) || !readExact(bookFile, &firstEntryPos, sizeof(firstEntryPos)) ||
      !bookFile.seek(firstEntryPos)) {
    LOG_DBG("BMC", "Could not seek to spine data for cumulative size cache; using SD lookups");
    return;
  }

  size_t fill = 0;
  size_t off = 0;
  // Copies (dst != nullptr) or skips the next n bytes of the stream, refilling the chunk as needed.
  const auto pull = [&](uint8_t* dst, size_t n) -> bool {
    while (n > 0) {
      if (off == fill) {
        const int got = bookFile.read(chunk.get(), CUMULATIVE_SIZE_READ_CHUNK);
        if (got <= 0) return false;
        fill = static_cast<size_t>(got);
        off = 0;
      }
      const size_t take = std::min(n, fill - off);
      if (dst) {
        memcpy(dst, chunk.get() + off, take);
        dst += take;
      }
      off += take;
      n -= take;
    }
    return true;
  };

  for (uint16_t i = 0; i < spineCount; ++i) {
    // Entry layout (writeSpineEntry): u32 hrefLen, href bytes, u32 cumulativeSize, i16 tocIndex.
    // Fields are copied out bytewise: they sit at unaligned offsets in the chunk.
    uint32_t hrefLen = 0;
    uint8_t raw[sizeof(uint32_t)];
    uint32_t size = 0;
    if (!pull(raw, sizeof(raw))) break;
    memcpy(&hrefLen, raw, sizeof(hrefLen));
    if (!pull(nullptr, hrefLen) || !pull(raw, sizeof(raw)) || !pull(nullptr, sizeof(int16_t))) break;
    memcpy(&size, raw, sizeof(size));
    sizes[i] = size;
    cumulativeSizeCount = static_cast<uint16_t>(i + 1);
  }

  if (cumulativeSizeCount != spineCount) {
    LOG_DBG("BMC", "Could not populate cumulative size cache; using SD lookups");
    cumulativeSizeCount = 0;
    return;
  }

  cumulativeSizes = std::move(sizes);
}

BookMetadataCache::CacheSize BookMetadataCache::getSpineCumulativeSize(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineCumulativeSize called but cache not loaded");
    return 0;
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineCumulativeSize index %d out of range", index);
    return 0;
  }

  if (!cumulativeSizesTried) {
    cacheSpineCumulativeSizes();
  }
  if (cumulativeSizes && index < static_cast<int>(cumulativeSizeCount)) {
    return cumulativeSizes[index];
  }

  // Uncached (spine over the cap, or no memory): LUT seek + entry seek, reading
  // only the size field rather than building the whole SpineEntry.
  uint32_t spineEntryPos = 0;
  uint32_t hrefLen = 0;
  CacheSize size = 0;
  if (!bookFile.seek(lutOffset + sizeof(uint32_t) * index) ||
      !readExact(bookFile, &spineEntryPos, sizeof(spineEntryPos)) || !bookFile.seek(spineEntryPos) ||
      !readExact(bookFile, &hrefLen, sizeof(hrefLen)) || !bookFile.seekCur(hrefLen) ||
      !readExact(bookFile, &size, sizeof(size))) {
    LOG_ERR("BMC", "Failed to read cumulative size for spine item %d", index);
    return 0;
  }
  return size;
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_ERR("BMC", "getTocEntry index %d out of range", index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * spineCount + sizeof(uint32_t) * index);
  uint32_t tocEntryPos;
  serialization::readPod(bookFile, tocEntryPos);
  bookFile.seek(tocEntryPos);
  return readTocEntry(bookFile);
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(FsFile& file) const {
  SpineEntry entry;
  serialization::readString(file, entry.href);
  serialization::readPod(file, entry.cumulativeSize);
  serialization::readPod(file, entry.tocIndex);
  return entry;
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(FsFile& file) const {
  TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}
