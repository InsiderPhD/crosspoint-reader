#pragma once
#include <cstdint>

class ReadingStatsStore {
  ReadingStatsStore() = default;
  static ReadingStatsStore instance;

 public:
  ReadingStatsStore(const ReadingStatsStore&) = delete;
  ReadingStatsStore& operator=(const ReadingStatsStore&) = delete;

  uint32_t totalReadingTimeSeconds = 0;
  uint32_t totalPagesRead = 0;
  uint32_t booksFinished = 0;
  uint32_t totalSessions = 0;

  static ReadingStatsStore& getInstance() { return instance; }

  void addReadingTime(uint32_t seconds);
  void addPageTurns(uint32_t count);
  void addBookFinished();
  void addSession();

  bool saveToFile() const;
  bool loadFromFile();
};

#define READING_STATS ReadingStatsStore::getInstance()
