/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "activities/home/ReadingStatsDetailActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long skipPageMs = 700;
}  // namespace

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());
  READING_STATS.beginSession(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());

  readingSessionStartMs = millis();
  sessionPageTurns = 0;

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  if (SETTINGS.readingSpeedSecondsPerPage > 0) {
    SETTINGS.saveToFile();
  }
  if (xtc && xtc->getPageCount() > 1) {
    const int clampedPage = static_cast<int>(std::min<uint32_t>(currentPage, xtc->getPageCount() - 1));
    const int progressPercent = static_cast<int>((clampedPage * 100) / (xtc->getPageCount() - 1));
    READING_STATS.updateProgress(static_cast<uint8_t>(std::clamp(progressPercent, 0, 100)), progressPercent >= 90);
  }
  READING_STATS.endSession();

  xtc.reset();
}

bool XtcReaderActivity::executeReaderAction(CrossPointSettings::READER_ACTION action) {
  using A = CrossPointSettings;
  switch (action) {
    case A::READER_ACTION_NONE:
      return false;

    case A::READER_ACTION_PAGE_FORWARD: {
      if (!xtc) return false;
      if (currentPage >= xtc->getPageCount()) {
        onGoHome();
        return true;
      }
      ReaderUtils::updateReadingSpeed(readingSpeedLastTurnMs);
      sessionPageTurns++;
      READING_STATS.noteActivity();
      currentPage++;
      if (currentPage >= xtc->getPageCount()) currentPage = xtc->getPageCount();
      requestUpdate();
      return false;
    }

    case A::READER_ACTION_PAGE_BACK: {
      if (!xtc) return false;
      if (currentPage >= xtc->getPageCount()) {
        currentPage = xtc->getPageCount() - 1;
        requestUpdate();
        return false;
      }
      ReaderUtils::updateReadingSpeed(readingSpeedLastTurnMs);
      sessionPageTurns++;
      READING_STATS.noteActivity();
      if (currentPage > 0) currentPage--;
      requestUpdate();
      return false;
    }

    case A::READER_ACTION_SKIP_CHAPTER_FORWARD: {
      if (!xtc) return false;
      if (currentPage >= xtc->getPageCount()) {
        onGoHome();
        return true;
      }
      ReaderUtils::updateReadingSpeed(readingSpeedLastTurnMs);
      sessionPageTurns++;
      READING_STATS.noteActivity();
      currentPage = (currentPage + 10 < xtc->getPageCount()) ? currentPage + 10 : xtc->getPageCount();
      requestUpdate();
      return false;
    }

    case A::READER_ACTION_SKIP_CHAPTER_BACK: {
      if (!xtc) return false;
      ReaderUtils::updateReadingSpeed(readingSpeedLastTurnMs);
      sessionPageTurns++;
      READING_STATS.noteActivity();
      currentPage = (currentPage >= 10) ? currentPage - 10 : 0;
      requestUpdate();
      return false;
    }

    case A::READER_ACTION_OPEN_MENU:
      if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
        startActivityForResult(
            std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                currentPage = std::get<PageResult>(result.data).page;
              }
            });
      }
      return false;

    case A::READER_ACTION_GO_HOME:
      onGoHome();
      return true;

    case A::READER_ACTION_FILE_BROWSER:
      activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
      return true;

    case A::READER_ACTION_SLEEP:
      enterDeepSleepFromReaderAction();
      return true;

    case A::READER_ACTION_FORCE_REFRESH:
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
      return false;

    case A::READER_ACTION_DARK_MODE:
      SETTINGS.darkMode = !SETTINGS.darkMode;
      SETTINGS.saveToFile();
      requestUpdate();
      return false;

    case A::READER_ACTION_READING_STATS:
      // Open the stats detail view for *this* book directly (not the full stats
      // overview), pushed on top of the reader via startActivityForResult so
      // Back returns to the book rather than Home.
      startActivityForResult(
          std::make_unique<ReadingStatsDetailActivity>(renderer, mappedInput, xtc->getPath(),
                                                       ReadingStatsDetailContext{.allowOpenBook = false}),
          [this](const ActivityResult&) { requestUpdate(); });
      return false;

    case A::READER_ACTION_MARK_FINISHED:
      if (xtc) {
        RECENT_BOOKS.updateProgress(xtc->getPath(), 100);
        RECENT_BOOKS.saveToFile();
      }
      READING_STATS.updateProgress(100, true);
      onGoHome();
      return true;

    default:
      return false;
  }
}

void XtcReaderActivity::loop() {
  READING_STATS.tickActiveSession();

  // ── Power: long press always sleeps; short press = configured action ──────
  if (mappedInput.isPressed(MappedInputManager::Button::Power) && mappedInput.getHeldTime() >= skipPageMs) {
    enterDeepSleepFromReaderAction();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    if (executeReaderAction(static_cast<CrossPointSettings::READER_ACTION>(SETTINGS.readerShortPressPower))) return;
  }

  // ── Confirm: short press ─────────────────────────────────────────────────
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (executeReaderAction(static_cast<CrossPointSettings::READER_ACTION>(SETTINGS.readerShortPressConfirm))) return;
  }

  // ── Back: long press / short press ───────────────────────────────────────
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= skipPageMs &&
      !longPressBackFired) {
    longPressBackFired = true;
    executeReaderAction(static_cast<CrossPointSettings::READER_ACTION>(SETTINGS.readerLongPressBack));
    return;
  }
  const bool longPressBackConsumed = longPressBackFired;
  if (!mappedInput.isPressed(MappedInputManager::Button::Back)) {
    longPressBackFired = false;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !longPressBackConsumed) {
    if (executeReaderAction(static_cast<CrossPointSettings::READER_ACTION>(SETTINGS.readerShortPressBack))) return;
  }

  // Resolve Left/Right with optional orientation swap.
  const bool swapFront =
      SETTINGS.frontButtonFollowOrientation && (SETTINGS.orientation == CrossPointSettings::INVERTED ||
                                                SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW);
  const auto shortPressLeft = static_cast<CrossPointSettings::READER_ACTION>(swapFront ? SETTINGS.readerShortPressRight
                                                                                       : SETTINGS.readerShortPressLeft);
  const auto longPressLeft = static_cast<CrossPointSettings::READER_ACTION>(swapFront ? SETTINGS.readerLongPressRight
                                                                                      : SETTINGS.readerLongPressLeft);
  const auto shortPressRight = static_cast<CrossPointSettings::READER_ACTION>(
      swapFront ? SETTINGS.readerShortPressLeft : SETTINGS.readerShortPressRight);
  const auto longPressRight = static_cast<CrossPointSettings::READER_ACTION>(swapFront ? SETTINGS.readerLongPressLeft
                                                                                       : SETTINGS.readerLongPressRight);

  // ── Left ─────────────────────────────────────────────────────────────────
  if (longPressLeft != CrossPointSettings::READER_ACTION_NONE) {
    if (mappedInput.isPressed(MappedInputManager::Button::Left) && mappedInput.getHeldTime() >= skipPageMs &&
        !longPressLeftFired) {
      longPressLeftFired = true;
      executeReaderAction(longPressLeft);
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Left)) longPressLeftFired = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) && mappedInput.getHeldTime() < skipPageMs) {
      if (executeReaderAction(shortPressLeft)) return;
    }
  } else {
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (executeReaderAction(shortPressLeft)) return;
    }
  }

  // ── Right ────────────────────────────────────────────────────────────────
  if (longPressRight != CrossPointSettings::READER_ACTION_NONE) {
    if (mappedInput.isPressed(MappedInputManager::Button::Right) && mappedInput.getHeldTime() >= skipPageMs &&
        !longPressRightFired) {
      longPressRightFired = true;
      executeReaderAction(longPressRight);
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Right)) longPressRightFired = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) && mappedInput.getHeldTime() < skipPageMs) {
      if (executeReaderAction(shortPressRight)) return;
    }
  } else {
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (executeReaderAction(shortPressRight)) return;
    }
  }

  // ── Side Up (PageBack) ───────────────────────────────────────────────────
  {
    const auto shortPressSideUp = static_cast<CrossPointSettings::READER_ACTION>(SETTINGS.readerShortPressSideUp);
    const auto longPressSideUp = static_cast<CrossPointSettings::READER_ACTION>(SETTINGS.readerLongPressSideUp);
    if (longPressSideUp != CrossPointSettings::READER_ACTION_NONE) {
      if (mappedInput.isPressed(MappedInputManager::Button::PageBack) && mappedInput.getHeldTime() >= skipPageMs &&
          !longPressPageBackFired) {
        longPressPageBackFired = true;
        executeReaderAction(longPressSideUp);
        return;
      }
      if (!mappedInput.isPressed(MappedInputManager::Button::PageBack)) longPressPageBackFired = false;
      if (mappedInput.wasReleased(MappedInputManager::Button::PageBack) && mappedInput.getHeldTime() < skipPageMs) {
        if (executeReaderAction(shortPressSideUp)) return;
      }
    } else {
      if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
        if (executeReaderAction(shortPressSideUp)) return;
      }
    }
  }

  // ── Side Down (PageForward) ──────────────────────────────────────────────
  {
    const auto shortPressSideDown = static_cast<CrossPointSettings::READER_ACTION>(SETTINGS.readerShortPressSideDown);
    const auto longPressSideDown = static_cast<CrossPointSettings::READER_ACTION>(SETTINGS.readerLongPressSideDown);
    if (longPressSideDown != CrossPointSettings::READER_ACTION_NONE) {
      if (mappedInput.isPressed(MappedInputManager::Button::PageForward) && mappedInput.getHeldTime() >= skipPageMs &&
          !longPressPageForwardFired) {
        longPressPageForwardFired = true;
        executeReaderAction(longPressSideDown);
        return;
      }
      if (!mappedInput.isPressed(MappedInputManager::Button::PageForward)) longPressPageForwardFired = false;
      if (mappedInput.wasReleased(MappedInputManager::Button::PageForward) && mappedInput.getHeldTime() < skipPageMs) {
        if (executeReaderAction(shortPressSideDown)) return;
      }
    } else {
      if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
        if (executeReaderAction(shortPressSideDown)) return;
      }
    }
  }

  // ── Tilt sensor: always page forward/back ────────────────────────────────
  if (SETTINGS.tiltPageTurn) {
    if (halTiltSensor.wasTiltedForward()) {
      executeReaderAction(CrossPointSettings::READER_ACTION_PAGE_FORWARD);
    } else if (halTiltSensor.wasTiltedBack()) {
      executeReaderAction(CrossPointSettings::READER_ACTION_PAGE_BACK);
    }
  }
}

void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title =
      SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = xtc->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& chapter) {
    return currentPage >= chapter.startPage && currentPage <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(currentPage - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(const StatusBarOverlayPosition position) const {
  const bool drawBottom = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom);
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  // Allocate page buffer
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu: bufferSize=%lu bitDepth=%u error=%s", currentPage, pageBufferSize,
            bitDepth, xtc::errorToString(xtc->getLastError()));
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2
    // - Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;              // Bit1 plane
    const uint8_t* plane2 = pageBuffer + planeSize;  // Bit2 plane
    const size_t colBytes = (pageHeight + 7) / 8;    // Bytes per column (100 for 800 height)

    // Lambda to get pixel value at (x, y)
    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    // Optimized grayscale rendering without storeBwBuffer (saves 48KB peak memory)
    // Flow: BW display → LSB/MSB passes → grayscale display → re-render BW for next frame

    // Count pixel distribution for debugging
    uint32_t pixelCounts[4] = {0, 0, 0, 0};
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        pixelCounts[getPixelValue(x, y)]++;
      }
    }
    LOG_DBG("XTR", "Pixel distribution: White=%lu, DarkGrey=%lu, LightGrey=%lu, Black=%lu", pixelCounts[0],
            pixelCounts[1], pixelCounts[2], pixelCounts[3]);

    // Pass 1: BW buffer - draw all non-white pixels as black
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayBuffer();
      pagesUntilFullRefresh--;
    }

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {  // Dark grey only
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {  // Dark grey or Light grey
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer (restore for next frame, instead of restoreBwBuffer)
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    free(pageBuffer);

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    return;
  } else {
    // 1-bit mode: 8 pixels per byte, MSB first
    const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        // Read source pixel (MSB first, bit 7 = leftmost pixel)
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  free(pageBuffer);

  if (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(StatusBarOverlayPosition::Top);
  } else {
    renderStatusBarOverlay(StatusBarOverlayPosition::Bottom);
  }

  // Display with appropriate refresh
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
}

void XtcReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    f.write(data, 4);
    f.close();
  }

  const uint32_t pageCount = xtc->getPageCount();
  if (pageCount > 0) {
    const auto progressPercent = static_cast<int8_t>(100u * currentPage / pageCount);
    RECENT_BOOKS.updateProgress(xtc->getPath(), progressPercent);
  }
}

void XtcReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    // Clamp to last valid page to avoid sentinel value (currentPage == pageCount)
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
