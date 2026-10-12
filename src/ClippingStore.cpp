#include "ClippingStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <functional>

#include "util/TimeUtils.h"

namespace {
// v2 appended bookFusionId after each record's timestamp; v3 follows it with
// the BookFusion start/end offsets and flags. Older files still load (missing
// fields read as 0) and are rewritten as v3 on the next save.
constexpr uint8_t VERSION = 3;
constexpr uint8_t VERSION_V1 = 1;
constexpr uint8_t VERSION_V2 = 2;

bool isReadableVersion(const uint8_t version) {
  return version == VERSION || version == VERSION_V2 || version == VERSION_V1;
}
constexpr size_t INITIAL_CLIPPING_RESERVE = 4;
constexpr char CLIPPINGS_DIR[] = "/.crosspoint/clippings";

// Key each book's store by a hash of its file path, matching how Epub/Txt/Xtc name their
// cache dirs (std::hash is deterministic within a firmware build, which is all we need).
std::string storeFilePathForBook(const std::string& filePath, const std::string& bookType) {
  const size_t hash = std::hash<std::string>{}(filePath);
  return std::string(CLIPPINGS_DIR) + "/" + bookType + "_" + std::to_string(hash) + ".bin";
}

void copyBounded(char* dst, const size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (!src) src = "";
  snprintf(dst, dstSize, "%s", src);
}

struct ClippingFileHeader {
  std::string title;
  std::string author;
  std::string path;
  std::string bookType;
  uint16_t count = 0;
};

bool readClippingFileHeader(const std::string& fullPath, const char* name, ClippingFileHeader& header) {
  FsFile f;
  if (!Storage.openFileForRead("CLIP", fullPath, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, version) || !isReadableVersion(version) || !serialization::tryReadPod(f, count) ||
      !serialization::tryReadString(f, header.title) || !serialization::tryReadString(f, header.author) ||
      !serialization::tryReadString(f, header.path)) {
    f.close();
    return false;
  }
  f.close();

  header.count = count;
  header.bookType = "epub";
  const std::string nameStr = name ? name : "";
  const size_t underscorePos = nameStr.find('_');
  if (underscorePos != std::string::npos) {
    header.bookType = nameStr.substr(0, underscorePos);
  }
  return true;
}
}  // namespace

ClippingStore ClippingStore::instance;

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  if (bookType != "epub") {
    LOG_ERR("CLIP", "Unknown clipping book type: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  clippings.clear();
  if (clippings.capacity() < INITIAL_CLIPPING_RESERVE) {
    clippings.reserve(INITIAL_CLIPPING_RESERVE);
  }

  storeFilePath = storeFilePathForBook(filePath, bookType);
  if (!Storage.exists(storeFilePath.c_str())) {
    return true;
  }

  return readFromFile();
}

void ClippingStore::unload() {
  if (dirty) saveToFile();
  clippings.clear();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
}

ClippingStore::AddResult ClippingStore::addClipping(const uint16_t spineIndex, const uint16_t startPage,
                                                    const uint16_t endPage, const uint16_t pageCount,
                                                    const uint16_t startWordIndex, const uint16_t endWordIndex,
                                                    const uint16_t wordCount, const char* chapterTitle,
                                                    const uint16_t paragraphIndex, const std::string& text) {
  if (clippings.size() >= CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping limit (%u) reached", CLIPPING_MAX_PER_BOOK);
    return AddResult::LimitReached;
  }

  Clipping clipping;
  clipping.spineIndex = spineIndex;
  clipping.startPage = startPage;
  clipping.endPage = endPage;
  clipping.pageCount = std::max<uint16_t>(1, pageCount);
  clipping.startWordIndex = startWordIndex;
  clipping.endWordIndex = endWordIndex;
  clipping.wordCount = wordCount;
  clipping.paragraphIndex = paragraphIndex;
  // UTC epoch seconds, or 0 while the clock has never been set. (This used to
  // store seconds since boot, which meant nothing after the next restart.)
  clipping.timestamp = TimeUtils::getCurrentValidTimestamp();
  copyBounded(clipping.chapterTitle, sizeof(clipping.chapterTitle), chapterTitle);
  // Keep the in-app store bounded. The full Kindle-style export is still written separately.
  clipping.text.assign(text.data(), std::min(text.size(), CLIPPING_TEXT_MAX));

  clippings.push_back(std::move(clipping));
  dirty = true;
  if (!saveToFile()) {
    clippings.pop_back();
    dirty = true;
    return AddResult::SaveFailed;
  }
  return AddResult::Added;
}

bool ClippingStore::removeClippingAt(const size_t index) {
  if (index >= clippings.size()) return false;
  Clipping clipping = std::move(clippings[index]);
  clippings.erase(clippings.begin() + index);
  dirty = true;
  if (!saveToFile()) {
    clippings.insert(clippings.begin() + index, std::move(clipping));
    dirty = true;
    return false;
  }
  return true;
}

bool ClippingStore::setBookFusionOffsets(const size_t index, const bool found, const uint32_t start,
                                         const uint32_t end) {
  if (index >= clippings.size()) return false;
  Clipping& clipping = clippings[index];
  clipping.bookFusionFlags |= BF_OFFSETS_RESOLVED;
  if (found) {
    clipping.bookFusionFlags |= BF_OFFSETS_FOUND;
    clipping.bookFusionStart = start;
    clipping.bookFusionEnd = end;
  }
  dirty = true;
  return true;
}

bool ClippingStore::setBookFusionPushed(const size_t index, const uint32_t id, const bool withOffsets) {
  if (index >= clippings.size()) return false;
  Clipping& clipping = clippings[index];
  clipping.bookFusionId = id;
  if (withOffsets) {
    clipping.bookFusionFlags |= BF_PUSHED_WITH_OFFSETS;
  } else {
    clipping.bookFusionFlags &= static_cast<uint8_t>(~BF_PUSHED_WITH_OFFSETS);
  }
  dirty = true;
  return true;
}

bool ClippingStore::hasClippingForPage(const uint16_t spineIndex, const uint16_t page) const {
  return std::any_of(clippings.begin(), clippings.end(), [&](const Clipping& clipping) {
    return clipping.spineIndex == spineIndex && page >= clipping.startPage && page <= clipping.endPage;
  });
}

bool ClippingStore::saveToFile() {
  if (!dirty) return true;
  if (writeToFile()) {
    dirty = false;
    return true;
  }
  return false;
}

void ClippingStore::clearAll() {
  clippings.clear();
  dirty = false;
  if (!storeFilePath.empty() && Storage.exists(storeFilePath.c_str())) {
    Storage.remove(storeFilePath.c_str());
  }
}

bool ClippingStore::readFromFile() { return readFromFile(storeFilePath, clippings); }

bool ClippingStore::readFromFile(const std::string& path, std::vector<Clipping>& out) const {
  out.clear();
  FsFile f;
  if (!Storage.openFileForRead("CLIP", path, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  std::string title;
  std::string author;
  std::string storedPath;
  if (!serialization::tryReadPod(f, version) || !isReadableVersion(version) || !serialization::tryReadPod(f, count) ||
      !serialization::tryReadString(f, title) || !serialization::tryReadString(f, author) ||
      !serialization::tryReadString(f, storedPath)) {
    f.close();
    LOG_ERR("CLIP", "Failed to read clipping header: %s", path.c_str());
    return false;
  }

  if (count > CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping count %u exceeds max, file may be corrupt: %s", count, path.c_str());
    f.close();
    return false;
  }

  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Clipping clipping;
    if (!serialization::tryReadPod(f, clipping.spineIndex) || !serialization::tryReadPod(f, clipping.startPage) ||
        !serialization::tryReadPod(f, clipping.endPage) || !serialization::tryReadPod(f, clipping.pageCount) ||
        !serialization::tryReadPod(f, clipping.startWordIndex) ||
        !serialization::tryReadPod(f, clipping.endWordIndex) || !serialization::tryReadPod(f, clipping.wordCount) ||
        !serialization::tryReadPod(f, clipping.paragraphIndex) || !serialization::tryReadPod(f, clipping.timestamp) ||
        (version >= VERSION_V2 && !serialization::tryReadPod(f, clipping.bookFusionId)) ||
        (version >= VERSION && (!serialization::tryReadPod(f, clipping.bookFusionStart) ||
                                !serialization::tryReadPod(f, clipping.bookFusionEnd) ||
                                !serialization::tryReadPod(f, clipping.bookFusionFlags)))) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at record %u: %s", i, path.c_str());
      return false;
    }
    if (f.read(reinterpret_cast<uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
        sizeof(clipping.chapterTitle)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at chapter title, record %u: %s", i, path.c_str());
      return false;
    }
    clipping.chapterTitle[sizeof(clipping.chapterTitle) - 1] = '\0';
    if (!serialization::tryReadString(f, clipping.text)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
      return false;
    }
    if (clipping.text.size() > CLIPPING_TEXT_MAX) {
      clipping.text.resize(CLIPPING_TEXT_MAX);
    }
    out.push_back(std::move(clipping));
  }

  f.close();
  return true;
}

bool ClippingStore::writeToFile() const {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CLIPPINGS_DIR);

  FsFile f = Storage.open(storeFilePath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!f) {
    LOG_ERR("CLIP", "Failed to open clipping file for write: %s", storeFilePath.c_str());
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(clippings.size(), CLIPPING_MAX_PER_BOOK));
  if (!serialization::tryWritePod(f, VERSION) || !serialization::tryWritePod(f, count) ||
      !serialization::tryWriteString(f, bookTitle) || !serialization::tryWriteString(f, bookAuthor) ||
      !serialization::tryWriteString(f, bookFilePath)) {
    LOG_ERR("CLIP", "Failed to write clipping header: %s", storeFilePath.c_str());
    f.close();
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const Clipping& clipping = clippings[i];
    if (!serialization::tryWritePod(f, clipping.spineIndex) || !serialization::tryWritePod(f, clipping.startPage) ||
        !serialization::tryWritePod(f, clipping.endPage) || !serialization::tryWritePod(f, clipping.pageCount) ||
        !serialization::tryWritePod(f, clipping.startWordIndex) ||
        !serialization::tryWritePod(f, clipping.endWordIndex) || !serialization::tryWritePod(f, clipping.wordCount) ||
        !serialization::tryWritePod(f, clipping.paragraphIndex) || !serialization::tryWritePod(f, clipping.timestamp) ||
        !serialization::tryWritePod(f, clipping.bookFusionId) ||
        !serialization::tryWritePod(f, clipping.bookFusionStart) ||
        !serialization::tryWritePod(f, clipping.bookFusionEnd) ||
        !serialization::tryWritePod(f, clipping.bookFusionFlags) ||
        f.write(reinterpret_cast<const uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
            sizeof(clipping.chapterTitle) ||
        !serialization::tryWriteString(f, clipping.text)) {
      LOG_ERR("CLIP", "Failed to write clipping record %u: %s", i, storeFilePath.c_str());
      f.close();
      return false;
    }
  }

  f.flush();
  f.close();
  return true;
}

bool ClippingStore::hasAnyClippings() {
  if (!Storage.exists(CLIPPINGS_DIR)) return false;
  // Emptiness only: stop at the first file instead of building up to 200 Strings.
  return !Storage.listFiles(CLIPPINGS_DIR, 1).empty();
}

bool ClippingStore::getAllClippedBooks(std::vector<ClippedBookEntry>& out) {
  if (!Storage.exists(CLIPPINGS_DIR)) return true;

  const auto files = Storage.listFiles(CLIPPINGS_DIR);
  for (const auto& name : files) {
    ClippingFileHeader header;
    const std::string fullPath = std::string(CLIPPINGS_DIR) + "/" + name.c_str();
    if (!readClippingFileHeader(fullPath, name.c_str(), header)) continue;
    if (header.path.empty() || header.count == 0 || !Storage.exists(header.path.c_str())) continue;

    auto existing = std::find_if(out.begin(), out.end(), [&](const ClippedBookEntry& entry) {
      return entry.bookPath == header.path && entry.bookType == header.bookType;
    });
    if (existing != out.end()) {
      existing->count = std::max(existing->count, header.count);
      continue;
    }
    out.push_back({std::move(header.title), std::move(header.author), std::move(header.path),
                   std::move(header.bookType), header.count});
  }
  return true;
}

bool ClippingStore::hasForFilePath(const std::string& filePath, const std::string& bookType) {
  return Storage.exists(storeFilePathForBook(filePath, bookType).c_str());
}

void ClippingStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string path = storeFilePathForBook(filePath, bookType);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
}

bool ClippingStore::moveForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                    const std::string& bookType) {
  const std::string oldStorePath = storeFilePathForBook(oldFilePath, bookType);
  if (!Storage.exists(oldStorePath.c_str())) {
    return true;
  }
  const std::string newStorePath = storeFilePathForBook(newFilePath, bookType);

  FsFile in;
  if (!Storage.openFileForRead("CLIP", oldStorePath, in)) {
    return false;
  }
  uint8_t version = 0;
  uint16_t count = 0;
  std::string title;
  std::string author;
  std::string storedPath;
  if (!serialization::tryReadPod(in, version) || !isReadableVersion(version) || !serialization::tryReadPod(in, count) ||
      !serialization::tryReadString(in, title) || !serialization::tryReadString(in, author) ||
      !serialization::tryReadString(in, storedPath)) {
    in.close();
    LOG_ERR("CLIP", "Failed to read clipping header: %s", oldStorePath.c_str());
    return false;
  }

  FsFile out = Storage.open(newStorePath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!out) {
    in.close();
    LOG_ERR("CLIP", "Failed to open clipping file for write: %s", newStorePath.c_str());
    return false;
  }

  // Same header with the new path, then the records byte for byte: their layout
  // depends on `version`, which is carried over unchanged, so nothing here has
  // to understand it and no clipping text is ever held in RAM.
  bool ok = serialization::tryWritePod(out, version) && serialization::tryWritePod(out, count) &&
            serialization::tryWriteString(out, title) && serialization::tryWriteString(out, author) &&
            serialization::tryWriteString(out, newFilePath);
  uint8_t chunk[128];
  while (ok) {
    const int got = in.read(chunk, sizeof(chunk));
    if (got < 0) ok = false;
    if (got <= 0) break;
    ok = out.write(chunk, static_cast<size_t>(got)) == static_cast<size_t>(got);
  }
  in.close();
  out.flush();
  out.close();

  if (!ok) {
    LOG_ERR("CLIP", "Failed to move clippings %s -> %s", oldStorePath.c_str(), newStorePath.c_str());
    Storage.remove(newStorePath.c_str());
    return false;
  }
  Storage.remove(oldStorePath.c_str());
  return true;
}

bool ClippingStore::migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author,
                                       const std::string& bookType) {
  const std::string oldStorePath = storeFilePathForBook(oldFilePath, bookType);
  if (!Storage.exists(oldStorePath.c_str())) {
    return true;
  }

  ClippingStore reader;
  std::vector<Clipping> migratedClippings;
  if (!reader.readFromFile(oldStorePath, migratedClippings)) {
    return false;
  }

  ClippingStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = storeFilePathForBook(newFilePath, bookType);
  writer.clippings = std::move(migratedClippings);
  if (!writer.writeToFile()) {
    return false;
  }

  Storage.remove(oldStorePath.c_str());
  return true;
}
