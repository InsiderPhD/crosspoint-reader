#include "ClippingStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>
#include <functional>

#include "util/TimeUtils.h"

namespace {
// /.crosspoint/clippings/ is shared with upstream CrossPoint and CrossInk. Both
// use a bare version byte, their numbers overlap ours with different record
// layouts, and upstream even uses our `<type>_<std::hash>.bin` names. So our
// files carry a "KPCL" magic before the version and live under a `kp_` prefix:
// nothing else writes either, and the other firmwares reject a 'K' version byte.
//
// Record layout by version: v2 appended bookFusionId after each record's
// timestamp; v3 follows it with the BookFusion start/end offsets and flags; v4
// then adds the clipping-sync fields (flags byte, server revision, id string);
// v5 adds the content offsets after the id. Missing fields take their defaults,
// so every old clip is pending upload.
//
// Files written before the magic (untagged `<type>_<hash>.bin`, v1..v5) still
// load: see openStore() for how they are told apart from upstream's.
constexpr char MAGIC[4] = {'K', 'P', 'C', 'L'};
constexpr uint8_t VERSION = 5;
constexpr uint8_t VERSION_V1 = 1;
constexpr uint8_t VERSION_V2 = 2;
constexpr uint8_t VERSION_V3 = 3;
constexpr uint8_t VERSION_V4 = 4;

// v4 record flags byte
constexpr uint8_t SYNC_PENDING_UPLOAD = 0x01;
constexpr uint8_t SYNC_FOREIGN_ANCHOR = 0x02;
constexpr uint8_t SYNC_OFFSETS_RESOLVED = 0x04;  // v5

// Bounds for parsing files we did not necessarily write: a garbage length must
// be rejected before it reaches std::string::resize (no exceptions: bad_alloc aborts).
constexpr uint32_t HEADER_STRING_MAX = 4096;
// Upstream CrossPoint 2.0.0 keeps up to 4KB of text and 1024 clips per book.
constexpr uint32_t FOREIGN_TEXT_MAX = 4096;
constexpr uint16_t FOREIGN_COUNT_MAX = 1024;

constexpr size_t INITIAL_CLIPPING_RESERVE = 4;
constexpr char CLIPPINGS_DIR[] = "/.crosspoint/clippings";
constexpr char STORE_PREFIX[] = "kp_";
constexpr size_t STORE_PREFIX_LEN = sizeof(STORE_PREFIX) - 1;

// Ids of synced clips deleted on this device, queued until the server has been
// told. Fixed-size records, so a torn append is detectable and trimmed.
std::string deletionJournalPath(const std::string& storePath) { return storePath + ".deleted"; }

bool trimDeletionJournalTail(FsFile& journal) {
  const size_t size = journal.size();
  const size_t tail = size % CLIPPING_ID_SIZE;
  if (tail == 0) return true;
  if (!journal.truncate(size - tail)) {
    LOG_ERR("CLIP", "Failed to recover clipping deletion journal");
    return false;
  }
  return true;
}

void moveDeletionJournal(const std::string& oldStorePath, const std::string& newStorePath) {
  const std::string from = deletionJournalPath(oldStorePath);
  if (!Storage.exists(from.c_str())) return;
  const std::string to = deletionJournalPath(newStorePath);
  if (Storage.exists(to.c_str())) Storage.remove(to.c_str());
  if (!Storage.rename(from.c_str(), to.c_str())) {
    LOG_ERR("CLIP", "Failed to move clipping deletion journal: %s", from.c_str());
  }
}

// Key each book's store by a hash of its file path, matching how Epub/Txt/Xtc name their
// cache dirs (std::hash is deterministic within a firmware build, which is all we need).
std::string hashedStoreName(const std::string& filePath, const std::string& bookType) {
  return bookType + "_" + std::to_string(std::hash<std::string>{}(filePath)) + ".bin";
}

std::string storeFilePathForBook(const std::string& filePath, const std::string& bookType) {
  return std::string(CLIPPINGS_DIR) + "/" + STORE_PREFIX + hashedStoreName(filePath, bookType);
}

// Where firmware before the magic header kept this book's clippings, and where
// upstream CrossPoint keeps them today.
std::string legacyStoreFilePathForBook(const std::string& filePath, const std::string& bookType) {
  return std::string(CLIPPINGS_DIR) + "/" + hashedStoreName(filePath, bookType);
}

// The untagged name matching a tagged store path; empty if it is not one.
std::string legacyPathForStore(const std::string& storePath) {
  const size_t dirLen = sizeof(CLIPPINGS_DIR) - 1;
  if (storePath.size() <= dirLen + 1 + STORE_PREFIX_LEN ||
      storePath.compare(dirLen + 1, STORE_PREFIX_LEN, STORE_PREFIX) != 0) {
    return {};
  }
  return std::string(CLIPPINGS_DIR) + "/" + storePath.substr(dirLen + 1 + STORE_PREFIX_LEN);
}

// Splits "<type>_<decimal hash>.bin" (the name without any kp_ prefix).
bool parseStoreName(const std::string& name, std::string& bookType, std::string& hashDigits) {
  constexpr char SUFFIX[] = ".bin";
  constexpr size_t SUFFIX_LEN = sizeof(SUFFIX) - 1;
  const size_t underscore = name.find('_');
  if (underscore == std::string::npos || underscore == 0 || name.size() <= underscore + 1 + SUFFIX_LEN ||
      name.compare(name.size() - SUFFIX_LEN, SUFFIX_LEN, SUFFIX) != 0) {
    return false;
  }
  hashDigits = name.substr(underscore + 1, name.size() - SUFFIX_LEN - underscore - 1);
  if (!std::all_of(hashDigits.begin(), hashDigits.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
    return false;
  }
  bookType = name.substr(0, underscore);
  return true;
}

void copyBounded(char* dst, const size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (!src) src = "";
  snprintf(dst, dstSize, "%s", src);
}

bool readBoundedString(FsFile& f, std::string& s, const uint32_t max) {
  uint32_t len = 0;
  if (!serialization::tryReadPod(f, len) || len > max || len > static_cast<uint32_t>(f.available())) return false;
  s.resize(len);
  return len == 0 || f.read(&s[0], len) == static_cast<int>(len);
}

// Clip text of `len` bytes: keeps at most CLIPPING_TEXT_MAX (cut on a UTF-8
// boundary) when `out` is set, and seeks over the rest.
bool readClipText(FsFile& f, const uint32_t len, std::string* out) {
  if (len > FOREIGN_TEXT_MAX || len > static_cast<uint32_t>(f.available())) return false;
  uint32_t kept = 0;
  if (out) {
    kept = std::min<uint32_t>(len, CLIPPING_TEXT_MAX);
    out->resize(kept);
    if (kept > 0 && f.read(&(*out)[0], kept) != static_cast<int>(kept)) return false;
    if (kept < len) out->resize(static_cast<size_t>(utf8SafeTruncateBuffer(out->data(), static_cast<int>(kept))));
  }
  return kept == len || f.seekCur(static_cast<int64_t>(len - kept));
}

// Fields every layout (ours, upstream's, CrossInk's) starts a record with.
bool readCommonFields(FsFile& f, Clipping& c) {
  return serialization::tryReadPod(f, c.spineIndex) && serialization::tryReadPod(f, c.startPage) &&
         serialization::tryReadPod(f, c.endPage) && serialization::tryReadPod(f, c.pageCount) &&
         serialization::tryReadPod(f, c.startWordIndex) && serialization::tryReadPod(f, c.endWordIndex) &&
         serialization::tryReadPod(f, c.wordCount) && serialization::tryReadPod(f, c.paragraphIndex) &&
         serialization::tryReadPod(f, c.timestamp);
}

bool readChapterTitle(FsFile& f, Clipping& c) {
  if (f.read(reinterpret_cast<uint8_t*>(c.chapterTitle), sizeof(c.chapterTitle)) != sizeof(c.chapterTitle)) {
    return false;
  }
  c.chapterTitle[sizeof(c.chapterTitle) - 1] = '\0';
  return true;
}

// One KatiePoint record of the given version (see the layout notes above).
bool readOwnRecord(FsFile& f, const uint8_t version, Clipping& c, const bool keepText) {
  if (!readCommonFields(f, c) || (version >= VERSION_V2 && !serialization::tryReadPod(f, c.bookFusionId)) ||
      (version >= VERSION_V3 &&
       (!serialization::tryReadPod(f, c.bookFusionStart) || !serialization::tryReadPod(f, c.bookFusionEnd) ||
        !serialization::tryReadPod(f, c.bookFusionFlags)))) {
    return false;
  }
  if (version >= VERSION_V4) {
    uint8_t syncFlags = 0;
    std::string id;
    if (!serialization::tryReadPod(f, syncFlags) || !serialization::tryReadPod(f, c.syncRevision) ||
        !readBoundedString(f, id, CLIPPING_ID_SIZE - 1)) {
      return false;
    }
    c.pendingUpload = (syncFlags & SYNC_PENDING_UPLOAD) != 0;
    c.foreignAnchor = (syncFlags & SYNC_FOREIGN_ANCHOR) != 0;
    c.offsetsResolved = (syncFlags & SYNC_OFFSETS_RESOLVED) != 0;
    copyBounded(c.id, sizeof(c.id), id.c_str());
  }
  if (version >= VERSION &&
      (!serialization::tryReadPod(f, c.startOffset) || !serialization::tryReadPod(f, c.endOffset))) {
    return false;
  }
  uint32_t textLen = 0;
  return readChapterTitle(f, c) && serialization::tryReadPod(f, textLen) &&
         readClipText(f, textLen, keepText ? &c.text : nullptr);
}

// One upstream CrossPoint 2.0.0 record, v2..v4 (its v1 is byte-identical to
// ours). v3 adds a layout signature, v4 the sync fields with a fixed 65-byte id;
// text is a u16 length + bytes. Read-only: we never write this layout.
bool readUpstreamRecord(FsFile& f, const uint8_t version, Clipping& c, const bool keepText) {
  uint32_t layoutSignature = 0;
  if (!readCommonFields(f, c) || (version >= VERSION_V3 && !serialization::tryReadPod(f, layoutSignature))) {
    return false;
  }
  if (version >= VERSION_V4) {
    uint8_t pending = 0;
    if (!serialization::tryReadPod(f, c.startOffset) || !serialization::tryReadPod(f, c.endOffset) ||
        !serialization::tryReadPod(f, c.syncRevision) || !serialization::tryReadPod(f, pending) || pending > 1 ||
        f.read(reinterpret_cast<uint8_t*>(c.id), sizeof(c.id)) != sizeof(c.id) || c.id[sizeof(c.id) - 1] != '\0') {
      return false;
    }
    // Sync ids are printable ASCII; anything else means this is not that layout.
    for (const char* p = c.id; *p; ++p) {
      if (*p < 0x21 || *p > 0x7E) return false;
    }
    c.pendingUpload = pending != 0;
    c.offsetsResolved = c.startOffset != UINT32_MAX;
  }
  // Its page/word anchors come from another renderer's pagination.
  c.foreignAnchor = true;
  uint16_t textLen = 0;
  return readChapterTitle(f, c) && serialization::tryReadPod(f, textLen) &&
         readClipText(f, textLen, keepText ? &c.text : nullptr);
}

enum class StoreOrigin : uint8_t {
  Tagged,        // KPCL magic: ours, current format
  LegacyOwn,     // untagged, provably written by an older KatiePoint
  LegacyShared,  // untagged v1: the same bytes in our firmware and upstream's
  Upstream,      // untagged upstream CrossPoint v2..v4: imported read-only
};

bool isOwnLayout(const StoreOrigin origin) { return origin != StoreOrigin::Upstream; }
// Ours to rewrite, move or delete. Shared and upstream files are only ever read.
bool isOwned(const StoreOrigin origin) { return origin == StoreOrigin::Tagged || origin == StoreOrigin::LegacyOwn; }

struct StoreHeader {
  StoreOrigin origin = StoreOrigin::Tagged;
  uint8_t version = 0;
  uint16_t count = 0;
  std::string title;
  std::string author;
  std::string path;
};

bool readRecord(FsFile& f, const StoreOrigin origin, const uint8_t version, Clipping& c, const bool keepText) {
  return isOwnLayout(origin) ? readOwnRecord(f, version, c, keepText) : readUpstreamRecord(f, version, c, keepText);
}

// True when `count` records of that layout parse and end exactly at EOF.
bool recordsFit(FsFile& f, const size_t start, const StoreOrigin origin, const uint8_t version, const uint16_t count) {
  if (!f.seekSet(start)) return false;
  Clipping scratch;
  for (uint16_t i = 0; i < count; ++i) {
    if (!readRecord(f, origin, version, scratch, false)) return false;
  }
  return f.position() == f.size();
}

// Opens a clippings file, decides whose it is and leaves `f` at the first
// record. False (file closed) for anything we cannot attribute with certainty:
// CrossInk files, corrupt files, and untagged v2..v4 files whose records parse
// as both our layout and upstream's (or neither). Skipping beats misparsing.
bool openStore(const std::string& path, FsFile& f, StoreHeader& h) {
  if (!Storage.openFileForRead("CLIP", path, f)) return false;

  char magic[sizeof(MAGIC)] = {};
  const bool tagged = f.read(reinterpret_cast<uint8_t*>(magic), sizeof(magic)) == static_cast<int>(sizeof(magic)) &&
                      memcmp(magic, MAGIC, sizeof(MAGIC)) == 0;
  if ((!tagged && !f.seekSet(0)) || !serialization::tryReadPod(f, h.version) || h.version < VERSION_V1 ||
      h.version > VERSION || !serialization::tryReadPod(f, h.count) ||
      !readBoundedString(f, h.title, HEADER_STRING_MAX) || !readBoundedString(f, h.author, HEADER_STRING_MAX) ||
      !readBoundedString(f, h.path, HEADER_STRING_MAX)) {
    f.close();
    return false;
  }
  const size_t recordsStart = f.position();

  if (tagged) {
    h.origin = StoreOrigin::Tagged;
  } else if (h.version == VERSION) {
    // Neither upstream (max 4) nor CrossInk (max 4) ever wrote a v5.
    h.origin = StoreOrigin::LegacyOwn;
  } else if (h.version == VERSION_V1) {
    h.origin = StoreOrigin::LegacyShared;
  } else {
    const bool own =
        h.count <= CLIPPING_MAX_PER_BOOK && recordsFit(f, recordsStart, StoreOrigin::LegacyOwn, h.version, h.count);
    const bool upstream =
        h.count <= FOREIGN_COUNT_MAX && recordsFit(f, recordsStart, StoreOrigin::Upstream, h.version, h.count);
    if (own == upstream || !f.seekSet(recordsStart)) {
      LOG_DBG("CLIP", "Skipping clippings file of unknown origin: %s", path.c_str());
      f.close();
      return false;
    }
    h.origin = own ? StoreOrigin::LegacyOwn : StoreOrigin::Upstream;
  }

  const uint16_t maxCount = isOwned(h.origin) ? CLIPPING_MAX_PER_BOOK : FOREIGN_COUNT_MAX;
  if (h.count > maxCount) {
    LOG_ERR("CLIP", "Clipping count %u exceeds max, file may be corrupt: %s", h.count, path.c_str());
    f.close();
    return false;
  }
  return true;
}

bool readStoreHeader(const std::string& path, StoreHeader& h) {
  FsFile f;
  if (!openStore(path, f, h)) return false;
  f.close();
  return true;
}

uint16_t visibleCount(const StoreHeader& h) { return std::min<uint16_t>(h.count, CLIPPING_MAX_PER_BOOK); }
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
  removeLegacyOnSave = false;
  clippings.clear();
  if (clippings.capacity() < INITIAL_CLIPPING_RESERVE) {
    clippings.reserve(INITIAL_CLIPPING_RESERVE);
  }

  storeFilePath = storeFilePathForBook(filePath, bookType);
  if (Storage.exists(storeFilePath.c_str())) {
    return readFromFile();
  }

  // No tagged file yet: pick up an untagged one at the shared name, if it is
  // provably ours or one of upstream's we can import. Anything else is left
  // alone and the book starts empty; our first save goes to the tagged name.
  const std::string legacyPath = legacyStoreFilePathForBook(filePath, bookType);
  if (!Storage.exists(legacyPath.c_str())) {
    return true;
  }
  bool owned = false;
  if (!readFromFile(legacyPath, clippings, &owned)) {
    clippings.clear();
    return true;
  }
  if (owned) {
    // Ours from before the magic: rewrite it tagged on the next save and drop
    // the untagged copy, which upstream would otherwise misparse.
    moveDeletionJournal(legacyPath, storeFilePath);
    removeLegacyOnSave = true;
    dirty = true;
  }
  return true;
}

void ClippingStore::unload() {
  if (dirty) saveToFile();
  clippings.clear();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
  removeLegacyOnSave = false;
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
  // A clip the server knows about must be tombstoned there too, so queue its id
  // before dropping it. If the journal write fails, nothing is removed.
  if (clippings[index].id[0]) {
    FsFile journal = Storage.open(deletionJournalPath(storeFilePath).c_str(), O_WRONLY | O_CREAT | O_APPEND);
    if (!journal || !trimDeletionJournalTail(journal)) {
      LOG_ERR("CLIP", "Failed to open clipping deletion journal");
      return false;
    }
    const size_t originalSize = journal.size();
    if (journal.write(reinterpret_cast<const uint8_t*>(clippings[index].id), CLIPPING_ID_SIZE) != CLIPPING_ID_SIZE) {
      if (!journal.truncate(originalSize)) LOG_ERR("CLIP", "Failed to roll back clipping deletion journal");
      journal.close();
      LOG_ERR("CLIP", "Failed to queue clipping deletion");
      return false;
    }
    journal.flush();
    journal.close();
  }
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

bool ClippingStore::setContentOffsets(const size_t index, const bool found, const uint32_t start, const uint32_t end) {
  if (index >= clippings.size()) return false;
  Clipping& clipping = clippings[index];
  clipping.offsetsResolved = true;
  if (found) {
    clipping.startOffset = start;
    clipping.endOffset = end;
    // Already on the server without them: send it again so other devices get the anchor.
    if (clipping.syncRevision != 0 && !clipping.foreignAnchor) clipping.pendingUpload = true;
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

bool ClippingStore::prepareSync() {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (auto& clipping : clippings) {
    if (clipping.id[0]) continue;
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i) {
      clipping.id[i * 2] = HEX_DIGITS[bytes[i] >> 4];
      clipping.id[i * 2 + 1] = HEX_DIGITS[bytes[i] & 0x0F];
    }
    clipping.id[sizeof(bytes) * 2] = '\0';
    dirty = true;
  }
  return saveToFile();
}

bool ClippingStore::finishUploads() {
  for (auto& clipping : clippings) {
    if (!clipping.pendingUpload) continue;
    clipping.pendingUpload = false;
    dirty = true;
  }
  return saveToFile();
}

bool ClippingStore::nextDeletion(uint32_t& offset, char (&id)[CLIPPING_ID_SIZE]) const {
  id[0] = '\0';
  const std::string path = deletionJournalPath(storeFilePath);
  if (!Storage.exists(path.c_str())) return true;
  FsFile journal = Storage.open(path.c_str(), O_RDWR);
  if (!journal || !trimDeletionJournalTail(journal)) return false;
  if (offset % CLIPPING_ID_SIZE != 0 || !journal.seek(offset)) {
    LOG_ERR("CLIP", "Invalid clipping deletion journal");
    return false;
  }
  while (offset < journal.size()) {
    if (journal.read(reinterpret_cast<uint8_t*>(id), CLIPPING_ID_SIZE) != static_cast<int>(CLIPPING_ID_SIZE)) {
      LOG_ERR("CLIP", "Failed to read clipping deletion");
      id[0] = '\0';
      return false;
    }
    offset += CLIPPING_ID_SIZE;
    id[CLIPPING_ID_SIZE - 1] = '\0';
    // Skip an id that is live again (re-pulled before the deletion went up).
    const bool live = std::any_of(clippings.begin(), clippings.end(),
                                  [&](const Clipping& clipping) { return strcmp(clipping.id, id) == 0; });
    if (!live && id[0]) {
      journal.close();
      return true;
    }
  }
  journal.close();
  id[0] = '\0';
  return true;
}

bool ClippingStore::finishDeletions() {
  const std::string path = deletionJournalPath(storeFilePath);
  if (!Storage.exists(path.c_str()) || Storage.remove(path.c_str())) return true;
  LOG_ERR("CLIP", "Failed to clear acknowledged clipping deletions");
  return false;
}

bool ClippingStore::applyRemote(const Clipping& remote, const bool deleted) {
  const auto it = std::find_if(clippings.begin(), clippings.end(),
                               [&](const Clipping& clipping) { return strcmp(clipping.id, remote.id) == 0; });
  if (it == clippings.end()) {
    if (deleted) return true;
    if (clippings.size() >= CLIPPING_MAX_PER_BOOK) {
      LOG_ERR("CLIP", "Remote clipping %s skipped: local limit (%u) reached", remote.id, CLIPPING_MAX_PER_BOOK);
      return true;
    }
    clippings.push_back(remote);
    dirty = true;
    if (!saveToFile()) {
      clippings.pop_back();
      dirty = false;
      return false;
    }
    return true;
  }

  const size_t index = static_cast<size_t>(it - clippings.begin());
  if (!deleted && it->syncRevision == remote.syncRevision && !it->pendingUpload) return true;

  Clipping previous = *it;
  if (deleted) {
    clippings.erase(it);
  } else {
    // BookFusion state is this device's alone; the server never sees it.
    Clipping merged = remote;
    merged.bookFusionId = previous.bookFusionId;
    merged.bookFusionStart = previous.bookFusionStart;
    merged.bookFusionEnd = previous.bookFusionEnd;
    merged.bookFusionFlags = previous.bookFusionFlags;
    // An offset lookup this device already ran still holds for the same text.
    if (merged.startOffset == UINT32_MAX && previous.offsetsResolved && previous.text == merged.text) {
      merged.startOffset = previous.startOffset;
      merged.endOffset = previous.endOffset;
      merged.offsetsResolved = true;
    }
    clippings[index] = std::move(merged);
  }
  dirty = true;
  if (saveToFile()) return true;
  if (deleted) {
    clippings.insert(clippings.begin() + index, std::move(previous));
  } else {
    clippings[index] = std::move(previous);
  }
  dirty = false;
  return false;
}

bool ClippingStore::hasClippingForPage(const uint16_t spineIndex, const uint16_t page) const {
  return std::any_of(clippings.begin(), clippings.end(), [&](const Clipping& clipping) {
    return clipping.spineIndex == spineIndex && page >= clipping.startPage && page <= clipping.endPage;
  });
}

bool ClippingStore::saveToFile() {
  if (!dirty) return true;
  if (!writeToFile()) return false;
  dirty = false;
  if (removeLegacyOnSave) {
    removeLegacyOnSave = false;
    const std::string legacyPath = legacyPathForStore(storeFilePath);
    if (!legacyPath.empty() && Storage.exists(legacyPath.c_str()) && !Storage.remove(legacyPath.c_str())) {
      LOG_ERR("CLIP", "Failed to remove migrated clipping file: %s", legacyPath.c_str());
    }
  }
  return true;
}

void ClippingStore::clearAll() {
  clippings.clear();
  dirty = false;
  if (storeFilePath.empty()) return;
  if (Storage.exists(storeFilePath.c_str())) {
    Storage.remove(storeFilePath.c_str());
  }
  const std::string journal = deletionJournalPath(storeFilePath);
  if (Storage.exists(journal.c_str())) {
    Storage.remove(journal.c_str());
  }
  const std::string legacyPath = legacyPathForStore(storeFilePath);
  if (legacyPath.empty() || !Storage.exists(legacyPath.c_str())) return;
  if (removeLegacyOnSave) {
    removeLegacyOnSave = false;
    Storage.remove(legacyPath.c_str());
  } else {
    // Another firmware's file we imported from: leave it, but shadow it with an
    // empty tagged file so the cleared clips are not imported again.
    dirty = true;
    saveToFile();
  }
}

bool ClippingStore::readFromFile() { return readFromFile(storeFilePath, clippings); }

bool ClippingStore::readFromFile(const std::string& path, std::vector<Clipping>& out, bool* owned) const {
  out.clear();
  FsFile f;
  StoreHeader h;
  if (!openStore(path, f, h)) {
    LOG_ERR("CLIP", "Failed to read clipping header: %s", path.c_str());
    return false;
  }
  if (owned) *owned = isOwned(h.origin);

  // Upstream allows more clips per book than we keep; the rest stay in its file.
  const uint16_t count = visibleCount(h);
  if (count < h.count) {
    LOG_INF("CLIP", "Importing %u of %u clippings: %s", count, h.count, path.c_str());
  }
  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Clipping clipping;
    if (!readRecord(f, h.origin, h.version, clipping, true)) {
      f.close();
      out.clear();
      LOG_ERR("CLIP", "Clipping file truncated at record %u: %s", i, path.c_str());
      return false;
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
  if (f.write(reinterpret_cast<const uint8_t*>(MAGIC), sizeof(MAGIC)) != sizeof(MAGIC) ||
      !serialization::tryWritePod(f, VERSION) || !serialization::tryWritePod(f, count) ||
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
        !serialization::tryWritePod(f, static_cast<uint8_t>((clipping.pendingUpload ? SYNC_PENDING_UPLOAD : 0) |
                                                            (clipping.foreignAnchor ? SYNC_FOREIGN_ANCHOR : 0) |
                                                            (clipping.offsetsResolved ? SYNC_OFFSETS_RESOLVED : 0))) ||
        !serialization::tryWritePod(f, clipping.syncRevision) ||
        !serialization::tryWriteString(f, std::string(clipping.id)) ||
        !serialization::tryWritePod(f, clipping.startOffset) || !serialization::tryWritePod(f, clipping.endOffset) ||
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
    // Only "kp_<type>_<hash>.bin" (ours) and "<type>_<hash>.bin" (shared with
    // upstream) are candidates; journals, upstream's .bak/.tmp sidecars and
    // anything else in the folder are skipped by name.
    const std::string fileName = name.c_str();
    const bool tagged = fileName.compare(0, STORE_PREFIX_LEN, STORE_PREFIX) == 0;
    const std::string baseName = tagged ? fileName.substr(STORE_PREFIX_LEN) : fileName;
    std::string bookType;
    std::string hashDigits;
    if (!parseStoreName(baseName, bookType, hashDigits)) continue;
    // Our tagged copy of the same book supersedes the untagged file.
    if (!tagged && Storage.exists((std::string(CLIPPINGS_DIR) + "/" + STORE_PREFIX + baseName).c_str())) continue;

    StoreHeader header;
    if (!readStoreHeader(std::string(CLIPPINGS_DIR) + "/" + fileName, header)) continue;
    if (tagged != (header.origin == StoreOrigin::Tagged)) continue;
    // An untagged name must be std::hash of the path it holds. CrossInk names its
    // files by crc32, and a stale path cannot be trusted either.
    if (!tagged && std::to_string(std::hash<std::string>{}(header.path)) != hashDigits) continue;
    if (header.path.empty() || header.count == 0 || !Storage.exists(header.path.c_str())) continue;

    const uint16_t count = visibleCount(header);
    auto existing = std::find_if(out.begin(), out.end(), [&](const ClippedBookEntry& entry) {
      return entry.bookPath == header.path && entry.bookType == bookType;
    });
    if (existing != out.end()) {
      existing->count = std::max(existing->count, count);
      continue;
    }
    out.push_back(
        {std::move(header.title), std::move(header.author), std::move(header.path), std::move(bookType), count});
  }
  return true;
}

bool ClippingStore::hasForFilePath(const std::string& filePath, const std::string& bookType) {
  if (Storage.exists(storeFilePathForBook(filePath, bookType).c_str())) return true;
  const std::string legacyPath = legacyStoreFilePathForBook(filePath, bookType);
  StoreHeader header;
  return Storage.exists(legacyPath.c_str()) && readStoreHeader(legacyPath, header);
}

uint16_t ClippingStore::countForFilePath(const std::string& filePath, const std::string& bookType) {
  std::string storePath = storeFilePathForBook(filePath, bookType);
  if (!Storage.exists(storePath.c_str())) {
    storePath = legacyStoreFilePathForBook(filePath, bookType);
    if (!Storage.exists(storePath.c_str())) return 0;
  }
  StoreHeader header;
  return readStoreHeader(storePath, header) ? visibleCount(header) : 0;
}

void ClippingStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string path = storeFilePathForBook(filePath, bookType);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
  const std::string journal = deletionJournalPath(path);
  if (Storage.exists(journal.c_str())) {
    Storage.remove(journal.c_str());
  }
  // An untagged file goes too, but only when it is provably ours.
  const std::string legacyPath = legacyStoreFilePathForBook(filePath, bookType);
  StoreHeader header;
  if (Storage.exists(legacyPath.c_str()) && readStoreHeader(legacyPath, header) && isOwned(header.origin)) {
    Storage.remove(legacyPath.c_str());
    const std::string legacyJournal = deletionJournalPath(legacyPath);
    if (Storage.exists(legacyJournal.c_str())) Storage.remove(legacyJournal.c_str());
  }
}

bool ClippingStore::moveForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                    const std::string& bookType) {
  std::string sourcePath = storeFilePathForBook(oldFilePath, bookType);
  const bool legacySource = !Storage.exists(sourcePath.c_str());
  if (legacySource) {
    sourcePath = legacyStoreFilePathForBook(oldFilePath, bookType);
    if (!Storage.exists(sourcePath.c_str())) return true;
  }
  const std::string newStorePath = storeFilePathForBook(newFilePath, bookType);

  FsFile in;
  StoreHeader header;
  if (!openStore(sourcePath, in, header)) {
    if (legacySource) return true;  // not ours to move
    LOG_ERR("CLIP", "Failed to read clipping header: %s", sourcePath.c_str());
    return false;
  }

  if (!isOwned(header.origin)) {
    // Imported from a file we must not rewrite: copy its clips to a tagged file
    // at the new path and leave the original where it is.
    in.close();
    ClippingStore reader;
    ClippingStore writer;
    if (!reader.readFromFile(sourcePath, writer.clippings)) return false;
    writer.bookFilePath = newFilePath;
    writer.bookTitle = std::move(header.title);
    writer.bookAuthor = std::move(header.author);
    writer.storeFilePath = newStorePath;
    return writer.writeToFile();
  }

  FsFile out = Storage.open(newStorePath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!out) {
    in.close();
    LOG_ERR("CLIP", "Failed to open clipping file for write: %s", newStorePath.c_str());
    return false;
  }

  // Tagged header with the new path, then the records byte for byte: their
  // layout depends on `version`, which is carried over unchanged (an untagged
  // file of ours gains the magic here), so nothing here has to understand it
  // and no clipping text is ever held in RAM.
  bool ok = out.write(reinterpret_cast<const uint8_t*>(MAGIC), sizeof(MAGIC)) == sizeof(MAGIC) &&
            serialization::tryWritePod(out, header.version) && serialization::tryWritePod(out, header.count) &&
            serialization::tryWriteString(out, header.title) && serialization::tryWriteString(out, header.author) &&
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
    LOG_ERR("CLIP", "Failed to move clippings %s -> %s", sourcePath.c_str(), newStorePath.c_str());
    Storage.remove(newStorePath.c_str());
    return false;
  }
  Storage.remove(sourcePath.c_str());
  moveDeletionJournal(sourcePath, newStorePath);
  return true;
}

bool ClippingStore::migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author,
                                       const std::string& bookType) {
  std::string oldStorePath = storeFilePathForBook(oldFilePath, bookType);
  const bool legacySource = !Storage.exists(oldStorePath.c_str());
  if (legacySource) {
    oldStorePath = legacyStoreFilePathForBook(oldFilePath, bookType);
    if (!Storage.exists(oldStorePath.c_str())) return true;
  }

  ClippingStore reader;
  std::vector<Clipping> migratedClippings;
  bool owned = false;
  if (!reader.readFromFile(oldStorePath, migratedClippings, &owned)) {
    return legacySource;  // an untagged file we cannot attribute is not ours to migrate
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

  if (owned) {
    Storage.remove(oldStorePath.c_str());
    moveDeletionJournal(oldStorePath, writer.storeFilePath);
  }
  return true;
}
