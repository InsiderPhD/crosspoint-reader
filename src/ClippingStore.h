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

struct Clipping {
  uint16_t spineIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint32_t timestamp = 0;
  // BookFusion highlight id once this clip has been pushed; 0 = not pushed yet.
  uint32_t bookFusionId = 0;
  // BookFusion anchor: UTF-16 offsets into the chapter's <body> text (end
  // exclusive). Valid only when BF_OFFSETS_FOUND is set.
  uint32_t bookFusionStart = 0;
  uint32_t bookFusionEnd = 0;
  uint8_t bookFusionFlags = 0;  // BF_* bits above
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
// /.crosspoint/clippings/. The in-app copy is bounded (CLIPPING_TEXT_MAX / CLIPPING_MAX_PER_BOOK);
// the full text is also appended to /My Clippings.txt by ClippingsManager.
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
  static void deleteForFilePath(const std::string& filePath, const std::string& bookType);
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

  bool readFromFile();
  bool readFromFile(const std::string& path, std::vector<Clipping>& out) const;
  bool writeToFile() const;
};

#define CLIPPINGS ClippingStore::getInstance()
