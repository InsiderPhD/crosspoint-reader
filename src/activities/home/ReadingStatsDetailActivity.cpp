#include "ReadingStatsDetailActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"

namespace {
constexpr int COVER_WIDTH = 96;
constexpr int COVER_HEIGHT = 140;
constexpr int PROGRESS_BLOCK_HEIGHT = 38;
constexpr int METRIC_CARD_HEIGHT = 64;
constexpr int METRIC_CARD_GAP = 8;
constexpr int DETAIL_FOCUS_ITEM_COUNT = 1;
constexpr size_t MAX_RESOLVED_COVERS = 16;
constexpr uint64_t MIN_ESTIMATE_READING_MS = 10ULL * 60ULL * 1000ULL;
constexpr uint8_t MIN_ESTIMATE_PROGRESS_PERCENT = 5;
constexpr uint64_t MIN_ESTIMATE_AVG_SESSION_MS = 5ULL * 60ULL * 1000ULL;
constexpr uint64_t ESTIMATE_ROUNDING_MS = 5ULL * 60ULL * 1000ULL;

struct BookCommitmentStats {
  uint32_t currentStreakDays = 0;
  uint32_t maxStreakDays = 0;
  uint8_t stickinessPercent = 0;
};

struct ResolvedCoverCacheEntry {
  std::string bookPath;
  std::string coverBmpPath;
  std::string resolvedPath;
};

std::vector<ResolvedCoverCacheEntry>& getResolvedCoverCache() {
  static std::vector<ResolvedCoverCacheEntry> cache;
  return cache;
}

std::string getCachedResolvedCoverPath(const ReadingBookStats& book) {
  auto& cache = getResolvedCoverCache();
  for (auto it = cache.begin(); it != cache.end(); ++it) {
    if (it->bookPath != book.path || it->coverBmpPath != book.coverBmpPath) {
      continue;
    }
    if (!it->resolvedPath.empty() && Storage.exists(it->resolvedPath.c_str())) {
      if (it != cache.begin()) {
        ResolvedCoverCacheEntry entry = *it;
        cache.erase(it);
        cache.insert(cache.begin(), std::move(entry));
      }
      return cache.front().resolvedPath;
    }
    break;
  }
  return "";
}

void rememberResolvedCoverPath(const ReadingBookStats& book, const std::string& resolvedPath) {
  if (resolvedPath.empty()) {
    return;
  }

  auto& cache = getResolvedCoverCache();
  cache.erase(std::remove_if(cache.begin(), cache.end(),
                             [&](const ResolvedCoverCacheEntry& entry) { return entry.bookPath == book.path; }),
              cache.end());
  cache.insert(cache.begin(), ResolvedCoverCacheEntry{book.path, book.coverBmpPath, resolvedPath});
  if (cache.size() > MAX_RESOLVED_COVERS) {
    cache.pop_back();
  }
}

ReadingBookStats withCoverPath(const ReadingBookStats& book, const std::string& coverBmpPath) {
  ReadingBookStats updated = book;
  updated.coverBmpPath = coverBmpPath;
  return updated;
}

const ReadingBookStats* findBook(const std::string& bookPath) {
  for (const auto& book : READING_STATS.getBooks()) {
    if (book.path == bookPath) {
      return &book;
    }
  }
  return nullptr;
}

std::string resolveStoredCoverPath(const std::string& coverBmpPath) {
  if (coverBmpPath.empty()) {
    return "";
  }

  if (coverBmpPath.find("[HEIGHT]") != std::string::npos) {
    const int candidateHeights[] = {COVER_HEIGHT, 160, 240, 400};
    for (const int height : candidateHeights) {
      const std::string resolved = UITheme::getCoverThumbPath(coverBmpPath, height);
      if (Storage.exists(resolved.c_str())) {
        return resolved;
      }
    }
    return "";
  }

  return Storage.exists(coverBmpPath.c_str()) ? coverBmpPath : "";
}

std::string ensureCoverPath(const ReadingBookStats& book) {
  const std::string cachedResolvedPath = getCachedResolvedCoverPath(book);
  if (!cachedResolvedPath.empty()) {
    return cachedResolvedPath;
  }

  std::string resolved = resolveStoredCoverPath(book.coverBmpPath);
  if (!resolved.empty()) {
    rememberResolvedCoverPath(book, resolved);
    return resolved;
  }

  if (!Storage.exists(book.path.c_str())) {
    return "";
  }

  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    if (!epub.load(true, true)) {
      return "";
    }
    epub.setupCacheDir();
    const std::string coverPath = epub.getCoverBmpPath();
    if (!Storage.exists(coverPath.c_str()) && !epub.generateCoverBmp()) {
      return "";
    }
    if (!Storage.exists(coverPath.c_str())) {
      return "";
    }
    READING_STATS.updateBookMetadata(book.path, epub.getTitle(), epub.getAuthor(), coverPath);
    rememberResolvedCoverPath(withCoverPath(book, coverPath), coverPath);
    return coverPath;
  }

  if (FsHelpers::hasXtcExtension(book.path)) {
    Xtc xtc(book.path, "/.crosspoint");
    if (!xtc.load()) {
      return "";
    }
    xtc.setupCacheDir();
    const std::string coverPath = xtc.getCoverBmpPath();
    if (!Storage.exists(coverPath.c_str()) && !xtc.generateCoverBmp()) {
      return "";
    }
    if (!Storage.exists(coverPath.c_str())) {
      return "";
    }
    READING_STATS.updateBookMetadata(book.path, xtc.getTitle(), xtc.getAuthor(), coverPath);
    rememberResolvedCoverPath(withCoverPath(book, coverPath), coverPath);
    return coverPath;
  }

  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    Txt txt(book.path, "/.crosspoint");
    if (!txt.load()) {
      return "";
    }
    txt.setupCacheDir();
    const std::string coverPath = txt.getCoverBmpPath();
    if (!Storage.exists(coverPath.c_str()) && !txt.generateCoverBmp()) {
      return "";
    }
    if (!Storage.exists(coverPath.c_str())) {
      return "";
    }
    READING_STATS.updateBookMetadata(book.path, txt.getTitle(), "", coverPath);
    rememberResolvedCoverPath(withCoverPath(book, coverPath), coverPath);
    return coverPath;
  }

  return "";
}

std::string findFastCoverPath(const ReadingBookStats& book) {
  const std::string cachedResolvedPath = getCachedResolvedCoverPath(book);
  if (!cachedResolvedPath.empty()) {
    return cachedResolvedPath;
  }

  std::string resolved = resolveStoredCoverPath(book.coverBmpPath);
  if (!resolved.empty()) {
    rememberResolvedCoverPath(book, resolved);
    return resolved;
  }

  if (!Storage.exists(book.path.c_str())) {
    return "";
  }

  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    resolved = resolveStoredCoverPath(epub.getCoverBmpPath());
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    Xtc xtc(book.path, "/.crosspoint");
    resolved = resolveStoredCoverPath(xtc.getCoverBmpPath());
  } else if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    Txt txt(book.path, "/.crosspoint");
    resolved = resolveStoredCoverPath(txt.getCoverBmpPath());
  }

  if (!resolved.empty()) {
    READING_STATS.updateBookMetadata(book.path, "", "", resolved);
    rememberResolvedCoverPath(withCoverPath(book, resolved), resolved);
  }
  return resolved;
}

std::string getDisplayTitle(const ReadingBookStats& book) { return book.title.empty() ? book.path : book.title; }

std::string formatDate(const uint32_t timestamp) {
  const std::string formatted = TimeUtils::formatDate(timestamp);
  return formatted.empty() ? std::string(tr(STR_NOT_SET)) : formatted;
}

std::string formatDateRange(const uint32_t startTimestamp, const uint32_t endTimestamp) {
  const std::string start = TimeUtils::formatDate(startTimestamp);
  const std::string end = TimeUtils::formatDate(endTimestamp);
  return (start.empty() ? "?" : start) + " - " + (end.empty() ? "?" : end);
}

uint32_t getCompletionDateForDisplay(const ReadingBookStats& book) { return book.completedAt; }

uint64_t roundUpEstimateMs(const uint64_t valueMs) {
  if (valueMs == 0) {
    return 0;
  }
  return ((valueMs + ESTIMATE_ROUNDING_MS - 1) / ESTIMATE_ROUNDING_MS) * ESTIMATE_ROUNDING_MS;
}

std::string buildSessionEstimateText(const uint64_t remainingMs, const ReadingBookStats& book) {
  if (book.sessions == 0) {
    return "";
  }

  const uint64_t averageSessionMs = book.totalReadingMs / book.sessions;
  if (averageSessionMs < MIN_ESTIMATE_AVG_SESSION_MS) {
    return "";
  }

  const uint32_t sessionsLeft =
      static_cast<uint32_t>((remainingMs + averageSessionMs - 1) / averageSessionMs);
  if (sessionsLeft == 0) {
    return "";
  }

  return std::to_string(sessionsLeft) + " " +
         (sessionsLeft == 1 ? std::string(tr(STR_SESSION)) : std::string(tr(STR_SESSIONS)));
}

std::string buildEstimatedTimeLeftText(const ReadingBookStats& book) {
  if (book.completed || book.lastProgressPercent >= 100) {
    return tr(STR_DONE);
  }

  if (book.totalReadingMs < MIN_ESTIMATE_READING_MS ||
      book.lastProgressPercent < MIN_ESTIMATE_PROGRESS_PERCENT) {
    return tr(STR_ESTIMATE_AFTER_MORE_READING);
  }

  const uint64_t estimatedTotalMs =
      (book.totalReadingMs * 100ULL + book.lastProgressPercent - 1) / book.lastProgressPercent;
  if (estimatedTotalMs <= book.totalReadingMs) {
    return tr(STR_ESTIMATE_AFTER_MORE_READING);
  }

  const uint64_t remainingMs = roundUpEstimateMs(estimatedTotalMs - book.totalReadingMs);
  std::string estimateText = "~" + ReadingStatsAnalytics::formatDurationHm(remainingMs);
  const std::string sessionText = buildSessionEstimateText(remainingMs, book);
  if (!sessionText.empty()) {
    estimateText += " / " + sessionText;
  }
  return estimateText;
}

BookCommitmentStats buildBookCommitmentStats(const ReadingBookStats& book) {
  BookCommitmentStats stats;

  if (!book.readingDays.empty()) {
    uint32_t run = 1;
    uint32_t best = 1;
    for (size_t index = 1; index < book.readingDays.size(); ++index) {
      if (book.readingDays[index].dayOrdinal == book.readingDays[index - 1].dayOrdinal + 1) {
        run++;
      } else {
        run = 1;
      }
      if (run > best) {
        best = run;
      }
    }
    stats.currentStreakDays = run;
    stats.maxStreakDays = best;
  }

  const uint64_t totalReadingMs = READING_STATS.getTotalReadingMs();
  if (totalReadingMs > 0) {
    const uint64_t percent = (book.totalReadingMs * 100ULL) / totalReadingMs;
    stats.stickinessPercent = static_cast<uint8_t>(std::min<uint64_t>(percent, 100ULL));
  }

  return stats;
}

void drawMetricCard(GfxRenderer& renderer, const Rect& rect, const char* label, const std::string& value) {
  constexpr int rowPadX = 14;
  constexpr int labelY = 10;
  constexpr int valueY = 34;
  const int textWidth = rect.width - rowPadX * 2;

  const std::string truncatedLabel =
      renderer.truncatedText(UI_10_FONT_ID, label, textWidth, EpdFontFamily::REGULAR);
  const std::string truncatedValue =
      renderer.truncatedText(UI_12_FONT_ID, value.c_str(), textWidth, EpdFontFamily::BOLD);

  renderer.drawText(UI_10_FONT_ID, rect.x + rowPadX, rect.y + labelY, truncatedLabel.c_str());
  renderer.drawText(UI_12_FONT_ID, rect.x + rowPadX, rect.y + valueY, truncatedValue.c_str(), true,
                    EpdFontFamily::BOLD);
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1);
}

Rect offsetRect(Rect rect, const int dy) {
  rect.y += dy;
  return rect;
}

void drawProgressBlock(GfxRenderer& renderer, const Rect& rect, const char* label, const uint8_t percent) {
  const std::string percentText = std::to_string(std::min<int>(percent, 100)) + "%";
  const int percentWidth = renderer.getTextWidth(UI_10_FONT_ID, percentText.c_str(), EpdFontFamily::BOLD);

  renderer.drawText(UI_10_FONT_ID, rect.x, rect.y, label);
  renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - percentWidth, rect.y, percentText.c_str(), true,
                    EpdFontFamily::BOLD);

  const Rect barRect{rect.x, rect.y + 23, rect.width, 10};
  renderer.drawRect(barRect.x, barRect.y, barRect.width, barRect.height);
  const int fillWidth = std::max(0, barRect.width - 4) * std::min<int>(percent, 100) / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barRect.x + 2, barRect.y + 2, fillWidth, std::max(0, barRect.height - 4));
  }
}

void drawCover(GfxRenderer& renderer, const Rect& rect, const std::string& coverPath) {
  const auto drawFallback = [&renderer, &rect]() {
    const char* label = tr(STR_BOOK);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int textY = rect.y + rect.height / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, true, EpdFontFamily::BOLD);
  };

  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  if (coverPath.empty()) {
    drawFallback();
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("RSD", coverPath, file)) {
    drawFallback();
    return;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() == BmpReaderError::Ok) {
    renderer.drawBitmap(bitmap, rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4);
  } else {
    drawFallback();
  }
  file.close();
}
}  // namespace

void ReadingStatsDetailActivity::onEnter() {
  Activity::onEnter();
  invalidateBaseScreenBuffer();
  resolvedCoverBmpPath.clear();
  coverLoadPending = false;
  selectedStatsItem = 0;
  scrollOffset = 0;
  maxScrollOffset = 0;
  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  waitForBackRelease = false;
  if (const auto* book = findBook(bookPath)) {
    resolvedCoverBmpPath = findFastCoverPath(*book);
    coverLoadPending = resolvedCoverBmpPath.empty();
  }
  requestUpdate();
}

void ReadingStatsDetailActivity::onExit() {
  Activity::onExit();
  freeBaseScreenBuffer();
}

bool ReadingStatsDetailActivity::storeBaseScreenBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  freeBaseScreenBuffer();

  const size_t bufferSize = renderer.getBufferSize();
  baseScreenBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!baseScreenBuffer) {
    return false;
  }

  memcpy(baseScreenBuffer, frameBuffer, bufferSize);
  baseScreenBufferStored = true;
  baseScreenBookPath = bookPath;
  baseScreenCoverPath = resolvedCoverBmpPath;
  baseScreenScrollOffset = scrollOffset;
  return true;
}

bool ReadingStatsDetailActivity::restoreBaseScreenBuffer() {
  if (!baseScreenBufferStored || !baseScreenBuffer || baseScreenBookPath != bookPath ||
      baseScreenCoverPath != resolvedCoverBmpPath || baseScreenScrollOffset != scrollOffset) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  memcpy(frameBuffer, baseScreenBuffer, renderer.getBufferSize());
  return true;
}

void ReadingStatsDetailActivity::invalidateBaseScreenBuffer() {
  baseScreenBufferStored = false;
  baseScreenBookPath.clear();
  baseScreenCoverPath.clear();
  baseScreenScrollOffset = -1;
}

void ReadingStatsDetailActivity::freeBaseScreenBuffer() {
  if (baseScreenBuffer) {
    free(baseScreenBuffer);
    baseScreenBuffer = nullptr;
  }
  invalidateBaseScreenBuffer();
}

void ReadingStatsDetailActivity::loop() {
  if (waitForBackRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      waitForBackRelease = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (waitForConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      waitForConfirmRelease = false;
    }
    return;
  }

  if (coverLoadPending) {
    coverLoadPending = false;
    if (const auto* book = findBook(bookPath)) {
      const std::string resolvedCoverPath = ensureCoverPath(*book);
      if (!resolvedCoverPath.empty() && resolvedCoverPath != resolvedCoverBmpPath) {
        resolvedCoverBmpPath = resolvedCoverPath;
        invalidateBaseScreenBuffer();
        requestUpdate();
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && Storage.exists(bookPath.c_str())) {
    onSelectBook(bookPath);
  }
}

void ReadingStatsDetailActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const auto* book = findBook(bookPath);

  if (!book) {
    renderer.clearScreen();
    invalidateBaseScreenBuffer();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS), nullptr);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, metrics.topPadding + metrics.headerHeight + 30,
                      tr(STR_NO_READING_STATS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int pageHeight = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int viewportBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect coverBaseRect{metrics.contentSidePadding, contentTop, COVER_WIDTH, COVER_HEIGHT};
  const int textX = coverBaseRect.x + coverBaseRect.width + 16;
  const int textWidth = pageWidth - textX - metrics.contentSidePadding;

  int currentY = contentTop + 6;
  const int titleTop = currentY;
  const auto wrappedTitle =
      renderer.wrappedText(UI_12_FONT_ID, getDisplayTitle(*book).c_str(), textWidth, 2, EpdFontFamily::BOLD);
  currentY += static_cast<int>(wrappedTitle.size()) * renderer.getLineHeight(UI_12_FONT_ID);

  const int authorTop = currentY + 4;
  if (!book->author.empty()) {
    currentY += renderer.getLineHeight(UI_10_FONT_ID) + 10;
  } else {
    currentY += 10;
  }

  currentY += 6;
  const int bookProgressTop = currentY;
  currentY += PROGRESS_BLOCK_HEIGHT + 14;
  const int chapterProgressTop = currentY;
  currentY += PROGRESS_BLOCK_HEIGHT + 14;
  const int chapterLabelTop = currentY;
  currentY += renderer.getLineHeight(UI_10_FONT_ID) + 6;
  const int chapterTextTop = currentY;
  const std::string currentChapter = book->chapterTitle.empty() ? std::string(tr(STR_NOT_SET)) : book->chapterTitle;
  const auto chapterLines =
      renderer.wrappedText(UI_10_FONT_ID, currentChapter.c_str(), textWidth, 2, EpdFontFamily::BOLD);
  currentY += static_cast<int>(chapterLines.size()) * renderer.getLineHeight(UI_10_FONT_ID);

  const int cardsTop = std::max(coverBaseRect.y + coverBaseRect.height, currentY) + metrics.verticalSpacing + 6;
  maxScrollOffset = 0;
  scrollOffset = 0;
  const int scrollDy = 0;
  const Rect coverRect = offsetRect(coverBaseRect, scrollDy);
  const BookCommitmentStats commitmentStats = buildBookCommitmentStats(*book);

  const bool baseScreenRestored = restoreBaseScreenBuffer();
  if (!baseScreenRestored) {
    renderer.clearScreen();
    drawCover(renderer, coverRect, resolvedCoverBmpPath);

    currentY = titleTop + scrollDy;
    for (const auto& line : wrappedTitle) {
      renderer.drawText(UI_12_FONT_ID, textX, currentY, line.c_str(), true, EpdFontFamily::BOLD);
      currentY += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!book->author.empty()) {
      renderer.drawText(UI_10_FONT_ID, textX, authorTop + scrollDy, book->author.c_str());
    }

    drawProgressBlock(renderer, Rect{textX, bookProgressTop + scrollDy, textWidth, PROGRESS_BLOCK_HEIGHT}, tr(STR_BOOK_PROGRESS),
                      book->lastProgressPercent);
    drawProgressBlock(renderer, Rect{textX, chapterProgressTop + scrollDy, textWidth, PROGRESS_BLOCK_HEIGHT}, tr(STR_CHAPTER_PROGRESS),
                      book->chapterProgressPercent);

    renderer.drawText(UI_10_FONT_ID, textX, chapterLabelTop + scrollDy, tr(STR_CURRENT_CHAPTER));

    currentY = chapterTextTop + scrollDy;
    for (const auto& line : chapterLines) {
      renderer.drawText(UI_10_FONT_ID, textX, currentY, line.c_str(), true, EpdFontFamily::BOLD);
      currentY += renderer.getLineHeight(UI_10_FONT_ID);
    }

    int drawCardsTop = cardsTop + scrollDy;

    const int cardWidth = (pageWidth - metrics.contentSidePadding * 2 - METRIC_CARD_GAP) / 2;

    drawMetricCard(renderer, Rect{metrics.contentSidePadding, drawCardsTop, cardWidth, METRIC_CARD_HEIGHT},
                   tr(STR_LAST_SESSION), ReadingStatsAnalytics::formatDurationHm(book->lastSessionMs));
    drawMetricCard(
        renderer,
        Rect{metrics.contentSidePadding + cardWidth + METRIC_CARD_GAP, drawCardsTop, cardWidth, METRIC_CARD_HEIGHT},
        tr(STR_TOTAL_TIME), ReadingStatsAnalytics::formatDurationHm(book->totalReadingMs));
    drawMetricCard(renderer,
                   Rect{metrics.contentSidePadding, drawCardsTop + METRIC_CARD_HEIGHT + METRIC_CARD_GAP, cardWidth,
                        METRIC_CARD_HEIGHT},
                   tr(STR_SESSIONS), std::to_string(book->sessions));
    drawMetricCard(renderer,
                   Rect{metrics.contentSidePadding + cardWidth + METRIC_CARD_GAP,
                        drawCardsTop + METRIC_CARD_HEIGHT + METRIC_CARD_GAP, cardWidth, METRIC_CARD_HEIGHT},
              tr(STR_BOOK_STREAK),
              std::to_string(commitmentStats.currentStreakDays) + " / " +
               std::to_string(commitmentStats.maxStreakDays));
    drawMetricCard(renderer,
                   Rect{metrics.contentSidePadding, drawCardsTop + (METRIC_CARD_HEIGHT + METRIC_CARD_GAP) * 2,
                        pageWidth - metrics.contentSidePadding * 2, METRIC_CARD_HEIGHT},
              tr(STR_BOOK_STICKINESS), std::to_string(commitmentStats.stickinessPercent) + "%");
    drawMetricCard(renderer,
                   Rect{metrics.contentSidePadding, drawCardsTop + (METRIC_CARD_HEIGHT + METRIC_CARD_GAP) * 3,
                        pageWidth - metrics.contentSidePadding * 2, METRIC_CARD_HEIGHT},
              tr(STR_START_END_DATE), formatDateRange(book->firstReadAt, getCompletionDateForDisplay(*book)));

    renderer.fillRect(0, 0, pageWidth, contentTop, false);
    if (viewportBottom < pageHeight) {
      renderer.fillRect(0, viewportBottom, pageWidth, pageHeight - viewportBottom, false);
    }
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS), nullptr);

    storeBaseScreenBuffer();
  }

  const char* confirmLabel = Storage.exists(bookPath.c_str()) ? tr(STR_OPEN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
