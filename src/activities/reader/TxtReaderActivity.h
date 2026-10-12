#pragma once

#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "ReaderActivity.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;
  unsigned long readingSpeedLastTurnMs = 0UL;
  unsigned long readingSessionStartMs = 0UL;
  uint32_t sessionPageTurns = 0;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  // Word / Character Spacing the page index was built with. Both move the wrap points, so
  // both are part of the index cache key.
  TextLayoutOptions cachedTextLayout;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  // Remembers this book as the one to wake into, once a page has rendered.
  RenderedBookGate openGate;
  // renderLockHeld: true from onExit(), which already runs under the RenderLock.
  void rememberBookOnceRendered(bool renderLockHeld);

  // Width and drawing of one display line under cachedTextLayout. With neutral spacing these
  // are the plain getTextWidth / drawText calls this reader has always made.
  int lineWidth(const char* line) const;
  void drawLine(int x, int y, const char* line) const;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("TxtReader", renderer, mappedInput), txt(std::move(txt)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  // Consumes tap zones via ReaderUtils::detectPageTurn (X4 Pro).
  bool consumesTouchInput() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
