#pragma once

#include <cstdint>
#include <string>
#include <vector>

inline constexpr size_t CLIPPING_CHAPTER_TITLE_MAX = 48;
inline constexpr size_t CLIPPING_TEXT_MAX = 512;
inline constexpr uint16_t CLIPPING_MAX_PER_BOOK = 64;

// Clipping::bookFusionFlags
inline constexpr uint8_t BF_OFFSETS_RESOLVED = 0x01;     // offset lookup ran to a verdict
inline constexpr uint8_t BF_OFFSETS_FOUND = 0x02;        // ...and found the text
inline constexpr uint8_t BF_PUSHED_WITH_OFFSETS = 0x04;  // bookFusionId was created with offsets

// Clipping::id capacity: up to 64 printable ASCII chars + NUL, the range the
// crosspoint-sync clippings extension accepts. Ids this firmware mints are 32
// hex chars; the extra room is so a clip minted elsewhere still round-trips.
inline constexpr size_t CLIPPING_ID_SIZE = 65;

struct Clipping {
  uint16_t spineIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  // When the clip was saved: UTC epoch seconds, 0 if the clock was not set.
  // Clips saved by firmware up to 1.12.0 hold seconds-since-boot here instead,
  // so treat a value TimeUtils::isClockValid() rejects as "unknown".
  uint32_t timestamp = 0;
  // BookFusion highlight id once this clip has been pushed; 0 = not pushed yet.
  uint32_t bookFusionId = 0;
  // BookFusion anchor: UTF-16 offsets into the chapter's <body> text (end
  // exclusive). Valid only when BF_OFFSETS_FOUND is set.
  uint32_t bookFusionStart = 0;
  uint32_t bookFusionEnd = 0;
  uint8_t bookFusionFlags = 0;  // BF_* bits above
  // crosspoint-sync clippings extension (see clippings/ClippingSync.cpp).
  // id is empty until the first sync mints one.
  char id[CLIPPING_ID_SIZE] = {};
  uint64_t syncRevision = 0;  // server revision last seen; 0 = never synced
  bool pendingUpload = true;  // not yet acknowledged by the server
  // Page/word anchors came from another device's layout, so they cannot be
  // trusted to land on the right words here: listed, but not highlighted.
  bool foreignAnchor = false;
  // Layout-independent anchor shared with every CrossPoint device: chapter
  // visible-codepoint offsets [start, end), counted the way the page parser
  // counts. UINT32_MAX = unknown. offsetsResolved: the lookup reached a verdict
  // (found or not), so it is not repeated on every sync.
  uint32_t startOffset = UINT32_MAX;
  uint32_t endOffset = UINT32_MAX;
  bool offsetsResolved = false;
  char chapterTitle[CLIPPING_CHAPTER_TITLE_MAX] = {};
  std::string text;
};

struct ClippedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  uint16_t count = 0;
};

// Per-book store of saved clippings, persisted as a small binary file under
// /.crosspoint/clippings/ (kp_<type>_<hash>.bin, "KPCL" magic; the folder is
// shared with upstream CrossPoint and CrossInk, whose files are never rewritten). The in-app copy is bounded
// (CLIPPING_TEXT_MAX / CLIPPING_MAX_PER_BOOK); the full text is also appended to /My Clippings.txt by ClippingsManager.
class ClippingStore {
 public:
  enum class AddResult : uint8_t {
    Added,
    LimitReached,
    SaveFailed,
  };

  static ClippingStore& getInstance() { return instance; }

  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  void unload();

  AddResult addClipping(uint16_t spineIndex, uint16_t startPage, uint16_t endPage, uint16_t pageCount,
                        uint16_t startWordIndex, uint16_t endWordIndex, uint16_t wordCount, const char* chapterTitle,
                        uint16_t paragraphIndex, const std::string& text);
  bool removeClippingAt(size_t index);
  // BookFusion sync bookkeeping. Both mark the store dirty but don't write —
  // callers update a batch of clips and then call saveToFile() once.
  bool setBookFusionOffsets(size_t index, bool found, uint32_t start, uint32_t end);
  bool setBookFusionPushed(size_t index, uint32_t id, bool withOffsets);
  // Record the content-offset lookup's verdict. A clip of this device's that
  // gains offsets after it was synced is queued to go up again with them.
  bool setContentOffsets(size_t index, bool found, uint32_t start, uint32_t end);
  // Clipping sync bookkeeping (clippings/ClippingSync.cpp). Deleting a clip that
  // has an id queues it in a per-book journal so the server can be told.
  bool prepareSync();      // mint ids for clips that have none, then save
  bool finishUploads();    // every pending clip was acknowledged, then save
  bool finishDeletions();  // every queued deletion was acknowledged
  // Next queued deletion at or after `offset` (advanced past it) whose id is not
  // also a live clip; id[0] == '\0' once the journal is exhausted.
  bool nextDeletion(uint32_t& offset, char (&id)[CLIPPING_ID_SIZE]) const;
  // Merge one server item: add it, replace the local copy (keeping BookFusion
  // state), or remove it. A clip over the per-book cap is skipped, not an error.
  bool applyRemote(const Clipping& clipping, bool deleted);
  bool saveToFile();
  void clearAll();

  bool hasClippings() const { return !clippings.empty(); }
  bool hasClippingForPage(uint16_t spineIndex, uint16_t page) const;
  const std::vector<Clipping>& getClippings() const { return clippings; }
  const std::string& getBookFilePath() const { return bookFilePath; }

  static bool hasAnyClippings();
  static bool getAllClippedBooks(std::vector<ClippedBookEntry>& out);
  // True when a clippings file exists on SD for this book (cheap existence check; nothing loaded).
  static bool hasForFilePath(const std::string& filePath, const std::string& bookType);
  // Saved-clipping count for this book from its file header only (nothing loaded); 0 if none.
  static uint16_t countForFilePath(const std::string& filePath, const std::string& bookType);
  static void deleteForFilePath(const std::string& filePath, const std::string& bookType);
  // The book file was renamed or moved: re-key its clippings file and rewrite
  // the path in its header, keeping the stored title/author. Streams the
  // records across (no clipping is loaded). True when there was nothing to move.
  static bool moveForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                              const std::string& bookType);
  static bool migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                 const std::string& title, const std::string& author, const std::string& bookType);

 private:
  static ClippingStore instance;

  std::vector<Clipping> clippings;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;

  // Set when the clips came from an untagged file of ours (written before the
  // magic header): the next save writes the tagged file and removes it.
  bool removeLegacyOnSave = false;

  bool readFromFile();
  // Reads any file we can attribute (see ClippingStore.cpp). `owned` is set when
  // it is ours to rewrite or delete; false for upstream / shared files.
  bool readFromFile(const std::string& path, std::vector<Clipping>& out, bool* owned = nullptr) const;
  bool writeToFile() const;
};

#define CLIPPINGS ClippingStore::getInstance()
