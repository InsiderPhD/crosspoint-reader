#include "ReadingStatsActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---- Static member definitions ----
bool ReadingStatsActivity::hasCachedData = false;
uint64_t ReadingStatsActivity::sCachedTotalBytes = 0;
int64_t ReadingStatsActivity::sCachedFreeBytes = -1;
int ReadingStatsActivity::sCachedEpubCount = 0;

namespace {

constexpr uint8_t STATS_CACHE_VERSION = 1;
constexpr char STATS_CACHE_PATH[] = "/.crosspoint/stats_cache.bin";

void formatDuration(uint32_t totalSeconds, char* buf, size_t bufSize) {
  const uint32_t mins = (totalSeconds + 30) / 60;
  if (mins < 60) {
    snprintf(buf, bufSize, "%um", static_cast<unsigned>(mins < 1 ? 0 : mins));
  } else if (mins < 60 * 24) {
    snprintf(buf, bufSize, "%uh %um", static_cast<unsigned>(mins / 60), static_cast<unsigned>(mins % 60));
  } else {
    const uint32_t hours = mins / 60;
    snprintf(buf, bufSize, "%ud %uh", static_cast<unsigned>(hours / 24), static_cast<unsigned>(hours % 24));
  }
}

void formatStorageSize(int64_t bytes, char* buf, size_t bufSize) {
  if (bytes < 0) {
    snprintf(buf, bufSize, "--");
  } else if (bytes >= 1000LL * 1024 * 1024) {
    snprintf(buf, bufSize, "%llu GB", static_cast<unsigned long long>(bytes / (1024ULL * 1024 * 1024)));
  } else {
    snprintf(buf, bufSize, "%llu MB", static_cast<unsigned long long>(bytes / (1024ULL * 1024)));
  }
}

// Try to read cached values from the file written by a previous boot session.
// Returns true and populates out-params on success.
bool tryLoadFileCache(uint64_t& outTotal, int64_t& outFree, int& outEpubs) {
  FsFile f;
  if (!Storage.openFileForRead("Stats", STATS_CACHE_PATH, f)) return false;
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != STATS_CACHE_VERSION) {
    f.close();
    return false;
  }
  int32_t epubs = 0;
  serialization::readPod(f, outTotal);
  serialization::readPod(f, outFree);
  serialization::readPod(f, epubs);
  f.close();
  outEpubs = static_cast<int>(epubs);
  return true;
}

void saveFileCache(uint64_t total, int64_t freeB, int epubs) {
  Storage.mkdir("/.crosspoint");
  FsFile f;
  if (!Storage.openFileForWrite("Stats", STATS_CACHE_PATH, f)) return;
  constexpr uint8_t version = STATS_CACHE_VERSION;
  serialization::writePod(f, version);
  serialization::writePod(f, total);
  serialization::writePod(f, freeB);
  const int32_t epubCount = static_cast<int32_t>(epubs);
  serialization::writePod(f, epubCount);
  f.close();
}

// Iterative BFS EPUB count — stack-safe, runs in background task
int countEpubsOnDevice() {
  int count = 0;
  std::vector<std::string> dirs;
  dirs.reserve(8);
  dirs.emplace_back("/");

  char name[128];
  while (!dirs.empty()) {
    const std::string path = std::move(dirs.back());
    dirs.pop_back();

    auto dir = Storage.open(path.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    dir.rewindDirectory();
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      file.getName(name, sizeof(name));
      if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
        file.close();
        continue;
      }
      if (file.isDirectory()) {
        dirs.push_back((path.back() == '/') ? path + name : path + "/" + name);
      } else if (FsHelpers::hasEpubExtension(std::string_view{name})) {
        count++;
      }
      file.close();
    }
    dir.close();
  }
  return count;
}

}  // namespace

// ---- Background compute task ----

void ReadingStatsActivity::computeTaskFunc(void* param) {
  auto* self = static_cast<ReadingStatsActivity*>(param);

  // Brief delay so the main task can render [Loading] before we start heavy I/O.
  vTaskDelay(pdMS_TO_TICKS(100));

  uint64_t total = 0;
  int64_t freeB = -1;
  int epubs = 0;

  // Fast path: try the persisted file cache from a previous boot session
  if (!tryLoadFileCache(total, freeB, epubs)) {
    // Slow path: compute fresh (freeClusterCount scans the FAT — can take seconds)
    total = Storage.totalBytes();
    freeB = Storage.freeBytes();
    epubs = countEpubsOnDevice();
    saveFileCache(total, freeB, epubs);
    LOG_DBG("Stats", "Computed fresh: total=%llu free=%lld epubs=%d",
            static_cast<unsigned long long>(total), static_cast<long long>(freeB), epubs);
  } else {
    LOG_DBG("Stats", "Loaded stats from file cache");
  }

  // Commit to static cache — all writes before setting hasCachedData
  sCachedTotalBytes = total;
  sCachedFreeBytes = freeB;
  sCachedEpubCount = epubs;
  hasCachedData = true;

  self->isLoading = false;
  // requestUpdate() BEFORE taskCompleted — onExit() may free self at any point after taskCompleted=true
  self->requestUpdate();
  self->taskCompleted = true;
  vTaskDelete(nullptr);
}

void ReadingStatsActivity::startComputeTask() {
  if (computeTaskHandle != nullptr && !taskCompleted) return;  // already running
  isLoading = true;
  taskCompleted = false;
  xTaskCreate(computeTaskFunc, "stats_cmp", 3072, this, 1, &computeTaskHandle);
}

// ---- Activity lifecycle ----

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  if (hasCachedData) {
    isLoading = false;  // instant — no I/O
  } else {
    startComputeTask();
  }
  requestUpdate();
}

void ReadingStatsActivity::onExit() {
  // Only vTaskDelete if the task hasn't already self-deleted.
  // taskCompleted is set true AFTER requestUpdate() and BEFORE vTaskDelete(nullptr) in the task,
  // so if true the handle is already invalid — do not touch it.
  if (computeTaskHandle != nullptr && !taskCompleted) {
    vTaskDelete(computeTaskHandle);
  }
  computeTaskHandle = nullptr;
  taskCompleted = false;
  Activity::onExit();
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !isLoading) {
    // Force recompute: clear both caches
    hasCachedData = false;
    Storage.remove(STATS_CACHE_PATH);
    startComputeTask();
    requestUpdate();
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  char timeBuf[16];
  formatDuration(READING_STATS.totalReadingTimeSeconds, timeBuf, sizeof(timeBuf));

  char pagesBuf[12];
  snprintf(pagesBuf, sizeof(pagesBuf), "%lu", static_cast<unsigned long>(READING_STATS.totalPagesRead));

  char finishedBuf[12];
  snprintf(finishedBuf, sizeof(finishedBuf), "%lu", static_cast<unsigned long>(READING_STATS.booksFinished));

  char sessionsBuf[12];
  snprintf(sessionsBuf, sizeof(sessionsBuf), "%lu", static_cast<unsigned long>(READING_STATS.totalSessions));

  char avgSessionBuf[16];
  if (READING_STATS.totalSessions > 0) {
    formatDuration(READING_STATS.totalReadingTimeSeconds / READING_STATS.totalSessions, avgSessionBuf,
                   sizeof(avgSessionBuf));
  } else {
    snprintf(avgSessionBuf, sizeof(avgSessionBuf), "--");
  }

  char speedBuf[16];
  if (SETTINGS.readingSpeedSecondsPerPage == 0) {
    snprintf(speedBuf, sizeof(speedBuf), "--");
  } else if (SETTINGS.readingSpeedSecondsPerPage < 60) {
    snprintf(speedBuf, sizeof(speedBuf), "~%us/pg", static_cast<unsigned>(SETTINGS.readingSpeedSecondsPerPage));
  } else {
    snprintf(speedBuf, sizeof(speedBuf), "~%um/pg", static_cast<unsigned>(SETTINGS.readingSpeedSecondsPerPage / 60));
  }

  int inProgressCount = 0;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.progressPercent > 0 && book.progressPercent < 90) inProgressCount++;
  }
  char inProgressBuf[8];
  snprintf(inProgressBuf, sizeof(inProgressBuf), "%d", inProgressCount);

  // Storage stats — "---" placeholder until background task completes
  char storageTotalBuf[12];
  char storageUsedBuf[12];
  char storageFreeBuf[12];
  char epubCountBuf[12];

  if (isLoading) {
    snprintf(storageTotalBuf, sizeof(storageTotalBuf), "---");
    snprintf(storageUsedBuf,  sizeof(storageUsedBuf),  "---");
    snprintf(storageFreeBuf,  sizeof(storageFreeBuf),  "---");
    snprintf(epubCountBuf,    sizeof(epubCountBuf),    "---");
  } else {
    const int64_t usedBytes = (sCachedFreeBytes >= 0 && sCachedTotalBytes > 0)
                                  ? static_cast<int64_t>(sCachedTotalBytes) - sCachedFreeBytes
                                  : -1;
    formatStorageSize(static_cast<int64_t>(sCachedTotalBytes), storageTotalBuf, sizeof(storageTotalBuf));
    formatStorageSize(usedBytes,            storageUsedBuf,  sizeof(storageUsedBuf));
    formatStorageSize(sCachedFreeBytes,     storageFreeBuf,  sizeof(storageFreeBuf));
    snprintf(epubCountBuf, sizeof(epubCountBuf), "%d", sCachedEpubCount);
  }

  const char* labels[] = {tr(STR_STATS_READING_TIME),     tr(STR_STATS_PAGES_READ),      tr(STR_STATS_BOOKS_FINISHED),
                          tr(STR_STATS_SESSIONS),          tr(STR_STATS_AVG_SESSION),     tr(STR_STATS_READING_SPEED),
                          tr(STR_STATS_BOOKS_IN_PROGRESS), tr(STR_STATS_EPUB_COUNT),
                          tr(STR_STATS_STORAGE_TOTAL),     tr(STR_STATS_STORAGE_USED),    tr(STR_STATS_STORAGE_FREE)};
  const char* values[] = {timeBuf,        pagesBuf,       finishedBuf,    sessionsBuf,   avgSessionBuf,
                          speedBuf,        inProgressBuf,  epubCountBuf,
                          storageTotalBuf, storageUsedBuf, storageFreeBuf};
  constexpr int ROW_COUNT = 11;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, ROW_COUNT, -1,
      [&labels](int i) { return std::string(labels[i]); }, nullptr, nullptr,
      [&values](int i) { return std::string(values[i]); });

  const auto hints = mappedInput.mapLabels(tr(STR_BACK), isLoading ? "" : tr(STR_REFRESH), "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  if (isLoading) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  } else {
    renderer.displayBuffer();
  }
}
