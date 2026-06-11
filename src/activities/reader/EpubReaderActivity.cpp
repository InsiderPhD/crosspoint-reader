#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_system.h>

#include "util/WifiTimeSync.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "BookFusionBookIdStore.h"
#include "BookFusionSyncActivity.h"
#include "BookFusionSyncClient.h"
#include "BookFusionTokenStore.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "JsonSettingsIO.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncActivity.h"
#include "KOReaderSyncClient.h"
#include "KOReaderSyncStateStore.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

// NOTE: This file used to wrap its helpers in an anonymous namespace.
// BookFusionSyncActivity needs to call makeBookFusionPosition,
// resolveBookFusionPosition, storedPositionFromBookFusion, and
// formatLocalSyncTimestamp; those four (and the helpers they depend on) are
// now externally linked so the activity can share them rather than
// duplicating the position math.

// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long bookmarkMessageDurationMs = 1500;
// pages per minute, first item is 1 to prevent division by zero if accessed
// Index 0 = Off, 1 = Auto (uses readingSpeedSecondsPerPage), 2+ = fixed durations
const std::vector<unsigned long> PAGE_TURN_DURATIONS_MS = {0,       0,       60000UL, 50000UL, 40000UL,
                                                           35000UL, 30000UL, 25000UL, 20000UL};

void enterDeepSleepFromReaderAction() {
  HalPowerManager::Lock powerLock;
  APP_STATE.lastSleepFromReader = true;
  APP_STATE.saveToFile();

  activityManager.goToSleep();
  display.deepSleep();
  LOG_DBG("READER", "Entering deep sleep from reader action");
  powerManager.startDeepSleep(gpio);
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

float clampUnit(const float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

// BookFusion's page_position_in_book uses the "start-of-page" convention:
//   page N of M chapter pages -> intra = N / M  (0-based N, so page 0 -> 0)
// The previous N / (M-1) endpoint convention caused the web reader to land
// one page ahead because its decoder does floor/round(intra * M). Match the
// convention so floor(intra * M) recovers the original page exactly.
float pageToIntraSpineProgress(const int pageNumber, const int totalPages) {
  if (totalPages <= 0) {
    return 0.0f;
  }
  const int clampedPage = std::max(0, std::min(pageNumber, totalPages - 1));
  return static_cast<float>(clampedPage) / static_cast<float>(totalPages);
}

bool makeBookFusionPosition(const std::shared_ptr<Epub>& epub, const int spineIndex, const int pageNumber,
                            const int totalPages, BookFusionPosition& out) {
  if (!epub) {
    return false;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0 || spineIndex < 0 || spineIndex >= spineCount) {
    return false;
  }

  const float intra = pageToIntraSpineProgress(pageNumber, totalPages);
  out.percentage = epub->calculateProgress(spineIndex, intra) * 100.0f;
  out.chapterIndex = spineIndex;
  out.pagePositionInBook = (static_cast<float>(spineIndex) + intra) / static_cast<float>(spineCount);
  return true;
}

bool resolveBookFusionPosition(const std::shared_ptr<Epub>& epub, const BookFusionPosition& pos, int& outSpineIndex,
                               float& outIntraSpineProgress) {
  if (!epub) {
    return false;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) {
    return false;
  }

  int spineIndex = pos.chapterIndex;
  if (spineIndex < 0 || spineIndex >= spineCount) {
    const float bookPos = clampUnit(pos.pagePositionInBook);
    spineIndex = static_cast<int>(bookPos * static_cast<float>(spineCount));
    if (spineIndex >= spineCount) {
      spineIndex = spineCount - 1;
    }
  }

  outSpineIndex = spineIndex;
  outIntraSpineProgress =
      clampUnit(pos.pagePositionInBook * static_cast<float>(spineCount) - static_cast<float>(spineIndex));
  return true;
}

BookFusionStoredPosition storedPositionFromBookFusion(const BookFusionPosition& pos) {
  BookFusionStoredPosition stored;
  stored.percentage = pos.percentage;
  stored.chapterIndex = pos.chapterIndex;
  stored.pagePositionInBook = pos.pagePositionInBook;
  // pageNumber/totalPages stay at sentinel; callers that know the local
  // chapter-relative page should overwrite them before persisting.
  return stored;
}

// Returns true if the current local reading position matches the stored
// last-synced position. When the stored sidecar has integer page coordinates
// (post-upgrade sync after a local upload), compare those exactly — that
// catches a 2-page advance that the float epsilon would otherwise swallow in
// books with many chapters. Falls back to the float epsilon for legacy
// sidecars and for stored positions written after an apply-remote (where the
// local page number isn't known at save time).
bool sameBookFusionPosition(const BookFusionStoredPosition& stored, const BookFusionPosition& pos, int localPageNumber,
                            int localTotalPages) {
  if (stored.chapterIndex != pos.chapterIndex) return false;
  if (stored.pageNumber >= 0 && stored.totalPages > 0 && localPageNumber >= 0 && localTotalPages > 0) {
    // Integer-exact comparison. totalPages can drift if the layout changed
    // (font/orientation change repaginated the chapter); when that happens
    // fall back to the float check rather than spuriously reporting "changed".
    if (stored.totalPages == localTotalPages) {
      return stored.pageNumber == localPageNumber;
    }
  }
  static constexpr float PAGE_POSITION_EPSILON = 0.0005f;
  static constexpr float PERCENTAGE_EPSILON = 0.05f;
  return std::fabs(stored.pagePositionInBook - pos.pagePositionInBook) <= PAGE_POSITION_EPSILON &&
         std::fabs(stored.percentage - pos.percentage) <= PERCENTAGE_EPSILON;
}

bool sameKOReaderPosition(const KOReaderStoredPosition& stored, const KOReaderPosition& pos, int localSpineIndex,
                          int localPageNumber, int localTotalPages) {
  if (stored.spineIndex != localSpineIndex) return false;
  if (stored.pageNumber >= 0 && stored.totalPages > 0 && localPageNumber >= 0 && localTotalPages > 0) {
    if (stored.totalPages == localTotalPages) {
      return stored.pageNumber == localPageNumber;
    }
  }
  static constexpr float KO_PERCENT_EPSILON = 0.0005f;
  return std::fabs(stored.percentage - pos.percentage) <= KO_PERCENT_EPSILON;
}

bool localEpochSeconds(int64_t& outEpoch) {
  outEpoch = 0;
  time_t now = time(nullptr);
  struct tm tm_utc;
  if (!gmtime_r(&now, &tm_utc)) return false;
  if (tm_utc.tm_year + 1900 < 2024) return false;
  outEpoch = static_cast<int64_t>(now);
  return true;
}

// Format the current device clock as an ISO 8601 UTC timestamp matching the
// shape BookFusion uses for `updated_at` (e.g. "2026-05-29T17:56:12.000Z").
// Caller must have NTP-synced the clock first — without that the formatted
// string is meaningless. Returns true on success, false if the clock looks
// uninitialised (year < 2024).
bool formatLocalSyncTimestamp(char* out, size_t outLen) {
  if (!out || outLen < 25) return false;
  time_t now = time(nullptr);
  struct tm tm_utc;
  if (!gmtime_r(&now, &tm_utc)) return false;
  if (tm_utc.tm_year + 1900 < 2024) return false;  // clock not synced
  // strftime can't emit ".000Z"; build it by hand to match server format
  // so string comparisons against remote updated_at sort correctly.
  snprintf(out, outLen, "%04d-%02d-%02dT%02d:%02d:%02d.000Z", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
           tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
  return true;
}

bool syncBookFusionTimeWithNTP() {
  // Delegate to the shared helper. Critical reason: this function used to call
  // esp_sntp_init() directly, which DID update the system clock (time(nullptr)
  // would return the synced time) but never flipped TimeUtils::syncedThisBoot
  // or persisted APP_STATE.lastKnownValidTimestamp. The result was that the
  // header date kept showing the "?" stale-clock prefix even after a
  // successful sync, because getAuthoritativeTimestamp() returned 0 and we
  // fell through to the persisted (now stale) timestamp. attemptIfStale() goes
  // through TimeUtils::syncTimeWithNtp which sets both.
  return WifiTimeSync::attemptIfStale();
}

void EpubReaderActivity::buildBookPageCache() {
  const int spineCount = epub->getSpineItemsCount();
  spinePageCountCache.clear();
  spinePageCountCache.reserve(spineCount);
  cachedTotalBookPages = 0;

  // First pass: read actual page counts from each cached section file header
  int totalKnownPages = 0;
  size_t totalKnownBytes = 0;
  for (int i = 0; i < spineCount; i++) {
    const std::string path = epub->getCachePath() + "/sections/" + std::to_string(i) + ".bin";
    spinePageCountCache.push_back(Section::readCachedPageCount(path));
    if (spinePageCountCache[i] > 0) {
      const size_t prevSize = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      totalKnownPages += spinePageCountCache[i];
      totalKnownBytes += epub->getCumulativeSpineItemSize(i) - prevSize;
    }
  }

  if (totalKnownBytes == 0) {
    return;  // No rendered chapters yet; time-left will fall back to old method
  }

  // Second pass: estimate uncached spines using pages-per-byte from known spines, sum total
  const float pagesPerByte = static_cast<float>(totalKnownPages) / static_cast<float>(totalKnownBytes);
  for (int i = 0; i < spineCount; i++) {
    if (spinePageCountCache[i] == 0) {
      const size_t prevSize = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      const size_t itemSize = epub->getCumulativeSpineItemSize(i) - prevSize;
      spinePageCountCache[i] =
          std::max<uint16_t>(1, static_cast<uint16_t>(pagesPerByte * static_cast<float>(itemSize)));
    }
    cachedTotalBookPages += spinePageCountCache[i];
  }
  LOG_DBG("ERS", "Book page cache built: %d total pages across %d spines (%d known)", cachedTotalBookPages, spineCount,
          totalKnownPages);
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  READING_STATS.beginSession(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  readingSessionStartMs = millis();
  sessionPageTurns = 0;

  buildBookPageCache();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  if (SETTINGS.readingSpeedSecondsPerPage > 0) {
    SETTINGS.saveToFile();
  }

  if (!bookFinishedRecorded && epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    const int progress = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
    if (progress >= 90) {
      bookFinishedRecorded = true;
      READING_STATS.updateProgress(100, true);
    }
  }
  READING_STATS.endSession();

  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  READING_STATS.tickActiveSession();

  // Record book completion here (main task) rather than in render() (render task), which would
  // race the stats store. Triggered once when the reader reaches the end-of-book position.
  if (!bookFinishedRecorded && currentSpineIndex >= epub->getSpineItemsCount()) {
    bookFinishedRecorded = true;
    READING_STATS.updateProgress(100, true);
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= bookmarkMessageDurationMs) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // Long-press Confirm: immediate action when threshold is reached (hold-based, not release-based)
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= skipChapterMs &&
      !longPressFeedbackShown) {
    longPressFeedbackShown = true;  // Prevent action spam

    if (SETTINGS.longPressAction == CrossPointSettings::LONG_PRESS_SYNC) {
      performLongPressSync();
    } else if (SETTINGS.longPressAction == CrossPointSettings::LONG_PRESS_PAGE_TURN) {
      pageTurn(true);
    } else if (SETTINGS.longPressAction == CrossPointSettings::LONG_PRESS_SLEEP) {
      enterDeepSleepFromReaderAction();
    } else if (SETTINGS.longPressAction == CrossPointSettings::LONG_PRESS_BOOKMARK) {
      const auto bookmarkResult = addBookmark();
      if (bookmarkResult != BookmarkToggleResult::None) {
        bookmarkMessageWasRemoval = (bookmarkResult == BookmarkToggleResult::Removed);
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
      }
    } else if (SETTINGS.longPressAction == CrossPointSettings::LONG_PRESS_NONE) {
      // No-op. longPressFeedbackShown stays true so the matching release won't
      // open the reader menu — holding Confirm with this setting means "do
      // nothing, keep reading".
    } else {
      // Default: full e-ink refresh to clear ghosting
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    }
    return;
  }

  // Snapshot before the reset below so we can tell whether the upcoming release
  // event terminates a long-press action (sync / refresh) — in that case the
  // release should NOT also open the reader menu; the user wants to stay on the
  // text and keep reading.
  const bool longPressActionConsumedRelease = longPressFeedbackShown;

  // Reset action flag when button is not pressed
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    longPressFeedbackShown = false;
  }

  // Enter reader menu activity (only on a short-press release).
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !longPressActionConsumedRelease) {
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

    uint32_t timeLeftChapter = 0;
    uint32_t timeLeftBook = 0;
    if (SETTINGS.readingSpeedSecondsPerPage > 0 && section) {
      const int pagesLeftChapter = totalPages - currentPage;
      if (pagesLeftChapter > 0) {
        timeLeftChapter = static_cast<uint32_t>(pagesLeftChapter) * SETTINGS.readingSpeedSecondsPerPage;
      }
      if (!spinePageCountCache.empty() && cachedTotalBookPages > 0) {
        int pagesBeforeChapter = 0;
        for (int i = 0; i < currentSpineIndex && i < static_cast<int>(spinePageCountCache.size()); i++) {
          pagesBeforeChapter += spinePageCountCache[i];
        }
        const int pagesLeftBook = cachedTotalBookPages - pagesBeforeChapter - currentPage;
        if (pagesLeftBook > 0) {
          timeLeftBook = static_cast<uint32_t>(pagesLeftBook) * SETTINGS.readingSpeedSecondsPerPage;
        }
      } else {
        // Fallback: estimate from current chapter's byte fraction of the book
        const float sectionStart = epub->calculateProgress(currentSpineIndex, 0.0f);
        const float sectionEnd = epub->calculateProgress(currentSpineIndex, 1.0f);
        const float sectionFraction = sectionEnd - sectionStart;
        const int pagesLeftBook = (sectionFraction > 0.001f)
                                      ? static_cast<int>((1.0f - bookProgress / 100.0f) / sectionFraction * totalPages)
                                      : pagesLeftChapter;
        if (pagesLeftBook > 0) {
          timeLeftBook = static_cast<uint32_t>(pagesLeftBook) * SETTINGS.readingSpeedSecondsPerPage;
        }
      }
    }

    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                               SETTINGS.orientation, !currentPageFootnotes.empty(), timeLeftChapter, timeLeftBook),
                           [this](const ActivityResult& result) {
                             // Always apply orientation change even if the menu was cancelled
                             const auto& menu = std::get<MenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             toggleAutoPageTurn(menu.pageTurnOption);
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN_SYNC) {
      performLongPressSync();
      return;
    }
    if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN_BOOKMARK) {
      const auto bookmarkResult = addBookmark();
      if (bookmarkResult != BookmarkToggleResult::None) {
        bookmarkMessageWasRemoval = (bookmarkResult == BookmarkToggleResult::Removed);
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
      }
      return;
    }
    if (SETTINGS.shortPwrBtn == CrossPointSettings::SLEEP) {
      enterDeepSleepFromReaderAction();
      return;
    }
  }

  if (ReaderUtils::detectAndApplyForceRefresh(mappedInput, renderer)) return;

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (skipChapter) {
    lastPageTurnTime = millis();
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      const int currentSpinePageCount = section ? section->pageCount : 0;
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath(), currentSpineIndex,
                                                        currentSpinePageCount),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& jump = std::get<SyncResult>(result.data);
                                 RenderLock lock(*this);
                                 currentSpineIndex = jump.spineIndex;
                                 nextPageNumber = jump.page;
                                 section.reset();
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::ADD_BOOKMARK: {
      const auto bookmarkResult = addBookmark();
      if (bookmarkResult != BookmarkToggleResult::None) {
        bookmarkMessageWasRemoval = (bookmarkResult == BookmarkToggleResult::Removed);
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::MARK_AS_COMPLETED: {
      if (epub) {
        RECENT_BOOKS.updateProgress(epub->getPath(), 100);
        RECENT_BOOKS.saveToFile();
      }
      if (!bookFinishedRecorded) {
        bookFinishedRecorded = true;
        READING_STATS.updateProgress(100, true);
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          saveProgress(backupSpine, backupPage, backupPageCount);
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC_PUSH:
    case EpubReaderMenuActivity::MenuAction::SYNC_PULL: {
      const bool isPush = (action == EpubReaderMenuActivity::MenuAction::SYNC_PUSH);
      std::string epubPath = epub->getPath();
      uint32_t bookId = BookFusionBookIdStore::loadBookId(epubPath.c_str());

      if (bookId != 0 && BF_TOKEN_STORE.hasToken()) {
        // BookFusion book with a valid account. Full-screen WiFi selection,
        // then full-screen sync activity. Mirrors the KOReader sync flow so
        // the two paths feel consistent.
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
        const int spineIndex = currentSpineIndex;
        const auto bfDirection =
            isPush ? BookFusionSyncActivity::Direction::PUSH : BookFusionSyncActivity::Direction::PULL;
        startActivityForResult(
            std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
            [this, bookId, spineIndex, currentPage, totalPages, bfDirection](const ActivityResult& result) {
              if (result.isCancelled) return;
              startActivityForResult(
                  std::make_unique<BookFusionSyncActivity>(renderer, mappedInput, epub, bookId, spineIndex, currentPage,
                                                           totalPages, bfDirection),
                  [this](const ActivityResult& syncResult) {
                    if (syncResult.isCancelled) return;
                    const auto& sync = std::get<SyncResult>(syncResult.data);
                    RenderLock lock(*this);
                    currentSpineIndex = sync.spineIndex;
                    nextPageNumber = 0;
                    cachedChapterTotalPageCount = 0;
                    pendingPageJump.reset();
                    if (sync.intraSpineProgress >= 0.0f) {
                      pendingSpineProgress = sync.intraSpineProgress;
                      pendingPercentJump = true;
                    } else {
                      nextPageNumber = sync.page;
                    }
                    section.reset();
                    pagesUntilFullRefresh = 1;  // full refresh on the post-sync redraw
                  });
            });
      } else if (KOREADER_STORE.hasCredentials()) {
        // KOReader fallback. KOReaderSyncActivity already runs its own full-
        // screen WiFi selection — pass the direction so it skips its result
        // comparison screen and performs the chosen direction immediately.
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
        std::optional<uint16_t> paragraphIndex;
        if (section && currentPage >= 0 && currentPage < section->pageCount) {
          const uint16_t paragraphPage =
              currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
          if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
            paragraphIndex = *pIdx;
          }
        }
        const auto direction = isPush ? KOReaderSyncActivity::Direction::PUSH : KOReaderSyncActivity::Direction::PULL;
        startActivityForResult(
            std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, epub, epub->getPath(), currentSpineIndex,
                                                   currentPage, totalPages, paragraphIndex, direction),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& sync = std::get<SyncResult>(result.data);
                if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
                  RenderLock lock(*this);
                  currentSpineIndex = sync.spineIndex;
                  nextPageNumber = sync.page;
                  cachedChapterTotalPageCount = 0;
                  pendingPageJump.reset();
                  saveProgress(currentSpineIndex, nextPageNumber, 0);
                  section.reset();
                }
              }
            });
      }
      break;
    }
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= PAGE_TURN_DURATIONS_MS.size()) {
    automaticPageTurnActive = false;
    autoPageTurnMode = false;
    return;
  }

  unsigned long duration;
  if (selectedPageTurnOption == 1) {
    // Auto mode: use calibrated reading speed; disable if uncalibrated
    if (SETTINGS.readingSpeedSecondsPerPage == 0) {
      automaticPageTurnActive = false;
      autoPageTurnMode = false;
      return;
    }
    duration = static_cast<unsigned long>(SETTINGS.readingSpeedSecondsPerPage) * 1000UL;
    autoPageTurnMode = true;
  } else {
    duration = PAGE_TURN_DURATIONS_MS[selectedPageTurnOption];
    autoPageTurnMode = false;
  }

  lastPageTurnTime = millis();
  pageTurnDuration = duration;
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  ReaderUtils::updateReadingSpeed(readingSpeedLastTurnMs);
  sessionPageTurns++;
  READING_STATS.noteActivity();
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }

  // Persist reading-stats progress every few turns, on the main task (never from render(), which
  // races the stats store). updateProgress() debounces the actual SD write, so this only bounds
  // the calculateProgress() recompute. `section` is null right after a cross-spine turn — skip
  // those; the next in-section turn (or onExit) records. onExit() captures the final exact value.
  static constexpr uint32_t STATS_PROGRESS_EVERY_N_TURNS = 3;
  if (section && (sessionPageTurns % STATS_PROGRESS_EVERY_N_TURNS) == 0) {
    recordStatsProgress();
  }

  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen. Do NOT record stats here: render() runs on the render task, and
  // mutating/saving READING_STATS off the main task races it and trips the storageMutex
  // priority-disinherit assert. Completion is recorded from loop() (main task) instead.
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.footnoteDisplay)) {
      LOG_DBG("ERS", "Cache not found, building...");

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getCodeFontId(),
                                      SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                      SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                      SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                      SETTINGS.footnoteDisplay, popupFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    // Update cache with the actual page count for this chapter now that it's loaded/rendered
    if (section->pageCount > 0 && currentSpineIndex < static_cast<int>(spinePageCountCache.size())) {
      const uint16_t oldEstimate = spinePageCountCache[currentSpineIndex];
      const uint16_t actualCount = section->pageCount;
      if (oldEstimate != actualCount) {
        cachedTotalBookPages += static_cast<int>(actualCount) - static_cast<int>(oldEstimate);
        spinePageCountCache[currentSpineIndex] = actualCount;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      return;
    }

    currentPageFootnotes = p->footnotes;

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkMessageWasRemoval ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
    if (SETTINGS.darkMode) {
      renderer.invertScreen();
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.footnoteDisplay)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(
          SETTINGS.getReaderFontId(), SETTINGS.getCodeFontId(), SETTINGS.getReaderLineCompression(),
          SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
          SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.footnoteDisplay)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  FsFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", spineIndex, currentPage);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }

  const float chapterProgress =
      (pageCount > 0) ? static_cast<float>(currentPage) / static_cast<float>(pageCount) : 0.0f;
  const auto progressPercent = static_cast<int8_t>(epub->calculateProgress(spineIndex, chapterProgress) * 100.0f);
  // saveProgress() is called from render() (render task) on every page. Mutating/saving
  // READING_STATS there races the main task and crashes (storageMutex priority-disinherit), so
  // only record stats progress from the main task. Reaching/ending the book is still recorded
  // via loop() and onExit(); reading time accrues via noteActivity() on page turns.
  if (!activityManager.isOnRenderTask()) {
    READING_STATS.updateProgress(
        static_cast<uint8_t>(std::clamp(static_cast<int>(progressPercent), 0, 100)), progressPercent >= 90, "",
        static_cast<uint8_t>(std::clamp(static_cast<int>((chapterProgress * 100.0f) + 0.5f), 0, 100)));
  }
  RECENT_BOOKS.updateProgress(epub->getPath(), progressPercent);
}

EpubReaderActivity::BookmarkToggleResult EpubReaderActivity::addBookmark() {
  // Bookmark toggle reads/writes SD files and page data; serialize with render()
  // to avoid storage mutex races between main and render tasks.
  if (activityManager.isOnRenderTask()) {
    LOG_ERR("ERS", "addBookmark() called on render task; ignoring");
    return BookmarkToggleResult::None;
  }

  RenderLock lock(*this);

  if (!section || !epub) {
    return BookmarkToggleResult::None;
  }

  const int currentPage = section->currentPage;
  const int pageCount = section->pageCount;
  if (currentPage < 0 || currentPage >= pageCount || pageCount <= 0) {
    return BookmarkToggleResult::None;
  }

  std::string pageText;
  if (auto page = section->loadPageFromSectionFile()) {
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) {
        continue;
      }
      const auto& line = static_cast<const PageLine&>(*el);
      if (!line.getBlock()) {
        continue;
      }
      const auto& words = line.getBlock()->getWords();
      for (const auto& word : words) {
        if (!pageText.empty()) {
          pageText += " ";
        }
        pageText += word;
      }
      if (pageText.size() > 300) {
        break;
      }
    }
  }

  CrossPointPosition pos = {currentSpineIndex, currentPage, pageCount};
  const KOReaderPosition koPos = ProgressMapper::toKOReader(epub, pos);

  BookmarkEntry entry;
  entry.percentage = koPos.percentage;
  entry.xpath = koPos.xpath;
  entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
  entry.computedSpineIndex = static_cast<uint16_t>(std::max(0, currentSpineIndex));
  entry.computedChapterPageCount = static_cast<uint16_t>(std::max(0, pageCount));
  entry.computedChapterProgress = static_cast<uint16_t>(std::max(0, currentPage));

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());

  std::vector<BookmarkEntry> bookmarks;
  if (Storage.exists(path.c_str())) {
    String json = Storage.readFile(path.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(bookmarks, json.c_str());
    }
  }

  auto isSameBookmark = [&entry](const BookmarkEntry& existing) {
    if (!entry.xpath.empty() && !existing.xpath.empty()) {
      return existing.xpath == entry.xpath;
    }

    // Fallback for legacy/partial entries: compare normalized book percentage.
    static constexpr float BOOKMARK_PERCENT_EPSILON = 0.0005f;
    return std::fabs(existing.percentage - entry.percentage) <= BOOKMARK_PERCENT_EPSILON;
  };

  const auto existingIt = std::find_if(bookmarks.begin(), bookmarks.end(), isSameBookmark);
  if (existingIt != bookmarks.end()) {
    bookmarks.erase(existingIt);
    if (!JsonSettingsIO::saveBookmarks(bookmarks, path.c_str())) {
      LOG_ERR("ERS", "Failed to save bookmark to %s", path.c_str());
      return BookmarkToggleResult::None;
    }
    return BookmarkToggleResult::Removed;
  } else {
    bookmarks.insert(bookmarks.begin(), entry);
    if (!JsonSettingsIO::saveBookmarks(bookmarks, path.c_str())) {
      LOG_ERR("ERS", "Failed to save bookmark to %s", path.c_str());
      return BookmarkToggleResult::None;
    }
    return BookmarkToggleResult::Added;
  }
}

void EpubReaderActivity::recordStatsProgress() {
  if (!epub || !section || section->pageCount == 0) {
    return;
  }
  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
  const auto progressPercent =
      static_cast<int8_t>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f);
  READING_STATS.updateProgress(
      static_cast<uint8_t>(std::clamp(static_cast<int>(progressPercent), 0, 100)), progressPercent >= 90, "",
      static_cast<uint8_t>(std::clamp(static_cast<int>((chapterProgress * 100.0f) + 0.5f), 0, 100)));
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  const int viewportBottom = renderer.getScreenHeight() - orientedMarginBottom;
  const int viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  const uint32_t heapBefore = esp_get_free_heap_size();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  if (SETTINGS.footnoteDisplay == CrossPointSettings::FOOTNOTE_ON_PAGE)
    page->renderFootnotes(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, viewportBottom, viewportWidth);
  scope.endScanAndPrewarm();
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing;

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  if (SETTINGS.footnoteDisplay == CrossPointSettings::FOOTNOTE_ON_PAGE)
    page->renderFootnotes(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, viewportBottom, viewportWidth);
  renderStatusBar();
  fcm->logStats("bw_render");
  const auto tBwRender = millis();

  if (imagePageWithAA) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      if (SETTINGS.footnoteDisplay == CrossPointSettings::FOOTNOTE_ON_PAGE)
        page->renderFootnotes(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, viewportBottom, viewportWidth);
      renderStatusBar();
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
  } else {
    if (SETTINGS.darkMode) renderer.invertScreen();
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  // Skipped in dark mode — AA LUT is computed for black-on-white.
  if (SETTINGS.textAntiAliasing && !SETTINGS.darkMode && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
        if (SETTINGS.footnoteDisplay == CrossPointSettings::FOOTNOTE_ON_PAGE)
          page->renderFootnotes(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, viewportBottom, viewportWidth);
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
        if (SETTINGS.footnoteDisplay == CrossPointSettings::FOOTNOTE_ON_PAGE)
          page->renderFootnotes(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, viewportBottom, viewportWidth);
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // Skipped in dark mode — AA LUT is computed for black-on-white.
    // TODO: Only do this if font supports it
    if (SETTINGS.textAntiAliasing && !SETTINGS.darkMode) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      renderer.storeBwBuffer();
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      if (SETTINGS.footnoteDisplay == CrossPointSettings::FOOTNOTE_ON_PAGE)
        page->renderFootnotes(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, viewportBottom, viewportWidth);
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      if (SETTINGS.footnoteDisplay == CrossPointSettings::FOOTNOTE_ON_PAGE)
        page->renderFootnotes(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, viewportBottom, viewportWidth);
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No anti-aliasing: BW frame already displayed above, no grayscale to
      // render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    if (autoPageTurnMode) {
      title = tr(STR_AUTO_TURN_ENABLED) + std::string("Auto");
    } else {
      title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(pageTurnDuration / 1000) + "s";
    }

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  uint32_t timeLeftSeconds = 0;
  if (SETTINGS.statusBarTimeLeft != CrossPointSettings::TIME_LEFT_HIDE && SETTINGS.readingSpeedSecondsPerPage > 0) {
    int pagesLeft = 0;
    if (SETTINGS.statusBarTimeLeft == CrossPointSettings::TIME_LEFT_CHAPTER) {
      pagesLeft = static_cast<int>(pageCount) - currentPage;
    } else if (!spinePageCountCache.empty() && cachedTotalBookPages > 0) {
      // Book: use actual per-chapter page counts from cache
      int pagesBeforeChapter = 0;
      for (int i = 0; i < currentSpineIndex && i < static_cast<int>(spinePageCountCache.size()); i++) {
        pagesBeforeChapter += spinePageCountCache[i];
      }
      pagesLeft = cachedTotalBookPages - pagesBeforeChapter - currentPage;
    } else {
      // Fallback: estimate from current chapter's byte fraction of the book
      const float sectionStart = epub->calculateProgress(currentSpineIndex, 0.0f);
      const float sectionEnd = epub->calculateProgress(currentSpineIndex, 1.0f);
      const float sectionFraction = sectionEnd - sectionStart;
      pagesLeft = (sectionFraction > 0.001f)
                      ? static_cast<int>((1.0f - bookProgress / 100.0f) / sectionFraction * pageCount)
                      : static_cast<int>(pageCount) - currentPage;
    }
    if (pagesLeft > 0) {
      timeLeftSeconds = static_cast<uint32_t>(pagesLeft) * SETTINGS.readingSpeedSecondsPerPage;
    }
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, timeLeftSeconds);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::performLongPressSync() {
  const std::string epubPath = epub ? epub->getPath() : std::string();
  const uint32_t bookId = epubPath.empty() ? 0 : BookFusionBookIdStore::loadBookId(epubPath.c_str());

  if (bookId != 0 && BF_TOKEN_STORE.hasToken()) {
    // Keep existing quick-sync behavior for BookFusion-linked books.
    connectWifiForSyncWithPopup([this]() { performBookFusionSync(); });
    return;
  }

  if (KOREADER_STORE.hasCredentials()) {
    // Run KOReader quick sync inline so long-press remains a one-gesture action
    // (same UX pattern as BookFusion quick sync).
    connectWifiForSyncWithPopup([this]() { performKOReaderQuickSync(); }, tr(STR_KOREADER_SYNC));
    return;
  }

  // No sync provider configured for this book/account state: preserve legacy
  // long-press behavior by doing a full refresh.
  RenderLock lock;
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void EpubReaderActivity::performKOReaderQuickSync() {
  if (!epub || !KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KRS", "KOReader quick sync unavailable, performing refresh instead");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  const std::string epubPath = epub->getPath();
  std::string documentHash;
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
  } else {
    documentHash = KOReaderDocumentId::calculate(epubPath);
  }
  if (documentHash.empty()) {
    LOG_DBG("KRS", "Failed to compute KOReader document hash, performing refresh instead");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  const KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);

  int64_t lastSyncTimestamp = 0;
  const bool hasLastSyncTimestamp = KOReaderSyncStateStore::loadLastSyncTimestamp(epubPath.c_str(), lastSyncTimestamp);
  KOReaderStoredPosition lastSyncedPosition;
  const bool hasLastSyncedPosition =
      KOReaderSyncStateStore::loadLastSyncedPosition(epubPath.c_str(), lastSyncedPosition);

  {
    RenderLock lock(*this);
    UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), "Fetching current time...");
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
  }
  const bool internetTimeSynced = syncBookFusionTimeWithNTP();

  {
    RenderLock lock(*this);
    UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), tr(STR_FETCH_PROGRESS));
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
  }

  KOReaderProgress remoteProgress;
  const auto downloadResult = KOReaderSyncClient::getProgress(documentHash, remoteProgress);

  bool shouldUploadProgress = true;
  bool appliedRemoteProgress = false;
  bool alreadyUpToDate = false;
  if (downloadResult == KOReaderSyncClient::OK) {
    const bool canCompareTimestamp = hasLastSyncTimestamp && remoteProgress.timestamp > 0;
    const bool canCompareSyncState = canCompareTimestamp && hasLastSyncedPosition;
    const bool remoteIsNewer = canCompareTimestamp && remoteProgress.timestamp > lastSyncTimestamp;
    const bool remoteIsFurtherAhead = remoteProgress.percentage > localKoPos.percentage + 0.01f;
    const bool localIsFurtherAhead = localKoPos.percentage > remoteProgress.percentage + 0.01f;
    const bool localChangedSinceLastSync =
        hasLastSyncedPosition && !sameKOReaderPosition(lastSyncedPosition, localKoPos, currentSpineIndex, currentPage, totalPages);

    if (canCompareSyncState) {
      LOG_DBG("KRS", "Last sync ts=%lld; remote ts=%lld; local changed=%d; localAhead=%d",
              static_cast<long long>(lastSyncTimestamp), static_cast<long long>(remoteProgress.timestamp),
              localChangedSinceLastSync ? 1 : 0, localIsFurtherAhead ? 1 : 0);
    } else if (canCompareTimestamp) {
      LOG_DBG("KRS", "Have sync timestamp but no stored sync position; falling back to furthest-ahead rule");
    } else {
      LOG_DBG("KRS", "No comparable sync timestamp; falling back to furthest-ahead rule");
    }

    const bool shouldApplyRemote =
        !localIsFurtherAhead && ((canCompareSyncState && remoteIsNewer && !localChangedSinceLastSync) ||
                                 (canCompareSyncState && remoteIsNewer && localChangedSinceLastSync &&
                                  !internetTimeSynced && remoteIsFurtherAhead) ||
                                 (!canCompareSyncState && remoteIsFurtherAhead));

    if (shouldApplyRemote) {
      KOReaderPosition remoteKoPos = {remoteProgress.progress, remoteProgress.percentage};
      const CrossPointPosition remotePos =
          ProgressMapper::toCrossPoint(epub, remoteKoPos, currentSpineIndex, std::max(1, totalPages));

      {
        RenderLock lock(*this);
        char msg[72];
        snprintf(msg, sizeof(msg), "Applying remote progress (%.0f%%)...", remoteProgress.percentage * 100.0f);
        UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), msg);
        if (SETTINGS.darkMode) renderer.invertScreen();
        renderer.displayBuffer();
      }

      currentSpineIndex = remotePos.spineIndex;
      nextPageNumber = std::max(0, remotePos.pageNumber);
      cachedChapterTotalPageCount = 0;
      pendingPageJump.reset();
      section.reset();
      shouldUploadProgress = false;
      appliedRemoteProgress = true;
      if (remoteProgress.timestamp > 0) {
        KOReaderSyncStateStore::saveLastSyncTimestamp(epubPath.c_str(), remoteProgress.timestamp);
      } else if (internetTimeSynced) {
        int64_t localTs = 0;
        if (localEpochSeconds(localTs)) {
          KOReaderSyncStateStore::saveLastSyncTimestamp(epubPath.c_str(), localTs);
        }
      }
      KOReaderStoredPosition stored;
      stored.percentage = remoteProgress.percentage;
      stored.spineIndex = remotePos.spineIndex;
      stored.pageNumber = remotePos.pageNumber;
      stored.totalPages = 0;
      KOReaderSyncStateStore::saveLastSyncedPosition(epubPath.c_str(), stored);
    } else if (canCompareSyncState && !remoteIsNewer && !localChangedSinceLastSync) {
      shouldUploadProgress = false;
      alreadyUpToDate = true;
    } else if (canCompareSyncState && remoteIsNewer && localChangedSinceLastSync && internetTimeSynced) {
      LOG_DBG("KRS", "Local position changed; uploading local progress with synced device time");
    } else if (canCompareSyncState && remoteIsNewer && localChangedSinceLastSync) {
      LOG_DBG("KRS", "Local and remote changed without internet time; using furthest-ahead fallback");
    } else if (localIsFurtherAhead && remoteIsNewer) {
      LOG_DBG("KRS", "Remote timestamp is newer but remote position is behind local; uploading local");
    }
  } else if (downloadResult == KOReaderSyncClient::NOT_FOUND) {
    LOG_DBG("KRS", "No remote KOReader progress found, uploading local position");
  } else {
    LOG_DBG("KRS", "Failed to fetch KOReader progress: %s", KOReaderSyncClient::errorString(downloadResult));
  }

  KOReaderSyncClient::Error uploadResult = KOReaderSyncClient::OK;
  if (shouldUploadProgress) {
    {
      RenderLock lock(*this);
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), tr(STR_UPLOAD_PROGRESS));
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }

    KOReaderProgress uploadProgress;
    uploadProgress.document = documentHash;
    uploadProgress.progress = localKoPos.xpath;
    uploadProgress.percentage = localKoPos.percentage;
    uploadResult = KOReaderSyncClient::updateProgress(uploadProgress);
    if (uploadResult == KOReaderSyncClient::OK) {
      if (internetTimeSynced) {
        int64_t localTs = 0;
        if (localEpochSeconds(localTs)) {
          KOReaderSyncStateStore::saveLastSyncTimestamp(epubPath.c_str(), localTs);
        }
      }
      KOReaderStoredPosition stored;
      stored.percentage = localKoPos.percentage;
      stored.spineIndex = currentSpineIndex;
      stored.pageNumber = currentPage;
      stored.totalPages = totalPages;
      KOReaderSyncStateStore::saveLastSyncedPosition(epubPath.c_str(), stored);
    }
  }

  {
    RenderLock lock(*this);
    if (appliedRemoteProgress) {
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), "Pulled remote progress to device");
    } else if (alreadyUpToDate) {
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), "Progress already up to date.");
    } else if (uploadResult == KOReaderSyncClient::OK) {
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), "Pushed local progress to server");
    } else {
      char errorMsg[96];
      snprintf(errorMsg, sizeof(errorMsg), "%s:\n%s", tr(STR_SYNC_FAILED_MSG),
               KOReaderSyncClient::errorString(uploadResult));
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), errorMsg);
    }
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
  }

  vTaskDelay(1200 / portTICK_PERIOD_MS);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  pagesUntilFullRefresh = 1;
  requestUpdateAndWait();
}

void EpubReaderActivity::connectWifiForSyncWithPopup(std::function<void()> onSuccess, const char* syncTitle) {
  const char* title = (syncTitle && syncTitle[0] != '\0') ? syncTitle : "BookFusion Sync";
  // Show connecting popup
  {
    RenderLock lock(*this);
    UITheme::drawSyncProgressPopup(renderer, title, "Connecting to WiFi...");
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
  }

  // Load WiFi credentials
  WIFI_STORE.loadFromFile();

  // Try to auto-connect to the last known network
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    // No saved networks, show error popup
    {
      RenderLock lock(*this);
      UITheme::drawSyncProgressPopup(renderer, title,
                                     "No WiFi networks configured.\nPlease set up WiFi in Settings first.");
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    return;
  }

  const auto* cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    // Network found but no credentials
    {
      RenderLock lock(*this);
      UITheme::drawSyncProgressPopup(renderer, title,
                                     "WiFi credentials not found.\nPlease reconnect in Settings.");
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    return;
  }

  // Attempt connection
  LOG_DBG("BFS", "Attempting to connect to WiFi: %s", lastSsid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }

  // Wait for connection with timeout
  int attempts = 0;
  const int maxAttempts = 100;  // 10 seconds

  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    attempts++;

    // Update popup every second
    if (attempts % 10 == 0) {
      RenderLock lock(*this);
      char statusMsg[64];
      snprintf(statusMsg, sizeof(statusMsg), "Connecting to WiFi...\n(%d/%d)", attempts / 10, maxAttempts / 10);
      UITheme::drawSyncProgressPopup(renderer, title, statusMsg);
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("BFS", "WiFi connected successfully");

    // Show success popup briefly
    {
      RenderLock lock(*this);
      UITheme::drawSyncProgressPopup(renderer, title, "WiFi connected!");
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Catch the NTP sync at the earliest possible moment after the connect.
    // performBookFusionSync also calls attemptIfStale later, but doing it here
    // means quick-syncs that fail mid-flow still leave us with a fresh
    // wall-clock for the rest of the boot.
    WifiTimeSync::attemptIfStale();

    // Proceed with sync
    onSuccess();
  } else {
    LOG_DBG("BFS", "WiFi connection failed");

    // Show failure popup
    {
      RenderLock lock(*this);
      UITheme::drawSyncProgressPopup(renderer, title,
                                     "WiFi connection failed.\nPlease check your settings.");
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

void EpubReaderActivity::performBookFusionSync() {
  // Check if this is a BookFusion book
  std::string epubPath = APP_STATE.openEpubPath;
  uint32_t bookId = BookFusionBookIdStore::loadBookId(epubPath.c_str());

  if (bookId == 0) {
    // Not a BookFusion book, fall back to refresh
    LOG_DBG("BFS", "Not a BookFusion book, performing refresh instead");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  // Check if we have a BookFusion token
  if (!BF_TOKEN_STORE.hasToken()) {
    // No token, fall back to refresh
    LOG_DBG("BFS", "No BookFusion token, performing refresh instead");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  // Perform bi-directional sync
  LOG_DBG("BFS", "Starting BookFusion sync for book %u", bookId);

  // Show initial sync popup
  {
    RenderLock lock(*this);
    UITheme::drawSyncProgressPopup(renderer, "BookFusion Sync", "Fetching current time...");
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
  }
  const bool internetTimeSynced = syncBookFusionTimeWithNTP();

  // Get current local position. section->currentPage is zero-based, which is
  // the coordinate system used by the reader and the sync mappers.
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;

  // Convert local position to BookFusion format
  BookFusionPosition localBfPos;
  if (!makeBookFusionPosition(epub, currentSpineIndex, currentPage, totalPages, localBfPos)) {
    LOG_DBG("BFS", "Could not build local BookFusion position, performing refresh instead");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }
  char lastSyncAt[40] = {};
  const bool hasLastSyncAt = BookFusionBookIdStore::loadLastSyncAt(epubPath.c_str(), lastSyncAt, sizeof(lastSyncAt));
  BookFusionStoredPosition lastSyncedPosition;
  const bool hasLastSyncedPosition =
      BookFusionBookIdStore::loadLastSyncedPosition(epubPath.c_str(), lastSyncedPosition);

  {
    RenderLock lock(*this);
    UITheme::drawSyncProgressPopup(renderer, "BookFusion Sync", "Downloading progress...");
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
  }

  // First, try to fetch remote progress from BookFusion
  BookFusionPosition remoteBfPos;
  auto downloadResult = BookFusionSyncClient::getProgress(bookId, remoteBfPos);

  bool shouldUploadProgress = true;
  bool appliedRemoteProgress = false;
  bool alreadyUpToDate = false;
  if (downloadResult == BookFusionSyncClient::OK) {
    LOG_DBG("BFS", "Remote progress: %.2f%%, Local progress: %.2f%%", remoteBfPos.percentage, localBfPos.percentage);

    const bool canCompareUpdatedAt = hasLastSyncAt && remoteBfPos.updatedAt[0] != '\0';
    const bool canCompareSyncState = canCompareUpdatedAt && hasLastSyncedPosition;
    const bool remoteIsNewer = canCompareUpdatedAt && strcmp(remoteBfPos.updatedAt, lastSyncAt) > 0;
    const bool remoteIsFurtherAhead = remoteBfPos.percentage > localBfPos.percentage + 1.0f;
    // Symmetric guard so a remote write that left the position behind local
    // (e.g. another device briefly opened the book and re-synced without
    // advancing) doesn't drag us backward just because its updated_at is later.
    const bool localIsFurtherAhead = localBfPos.percentage > remoteBfPos.percentage + 1.0f;
    const bool localChangedSinceLastSync =
        hasLastSyncedPosition && !sameBookFusionPosition(lastSyncedPosition, localBfPos, currentPage, totalPages);

    if (canCompareSyncState) {
      LOG_DBG("BFS", "Last synced at %s; remote updated_at=%s; local changed=%d; localAhead=%d", lastSyncAt,
              remoteBfPos.updatedAt, localChangedSinceLastSync ? 1 : 0, localIsFurtherAhead ? 1 : 0);
    } else if (canCompareUpdatedAt) {
      LOG_DBG("BFS", "Have sync timestamp but no stored sync position; falling back to furthest-ahead rule");
    } else {
      LOG_DBG("BFS", "No comparable sync timestamp; falling back to furthest-ahead rule");
    }

    // Never drag local backward: if local is meaningfully ahead of remote, keep
    // local and upload. This overrides the "remote newer wins" rule because a
    // newer timestamp on a behind-position is most often a stale write from
    // another device that opened the book without advancing.
    const bool shouldApplyRemote =
        !localIsFurtherAhead && ((canCompareSyncState && remoteIsNewer && !localChangedSinceLastSync) ||
                                 (canCompareSyncState && remoteIsNewer && localChangedSinceLastSync &&
                                  !internetTimeSynced && remoteIsFurtherAhead) ||
                                 (!canCompareSyncState && remoteIsFurtherAhead));

    if (shouldApplyRemote) {
      if (canCompareSyncState && remoteIsNewer && !localChangedSinceLastSync) {
        LOG_DBG("BFS", "Remote progress is newer and local is unchanged, updating local position");
      } else if (canCompareSyncState) {
        LOG_DBG("BFS", "Internet time unavailable; remote is further ahead, updating local position");
      } else {
        LOG_DBG("BFS", "Remote progress ahead, updating local position");
      }

      // Show direction-clear popup so the user can see we're pulling FROM
      // BookFusion (rather than the opposite).
      {
        RenderLock lock(*this);
        char msg[64];
        snprintf(msg, sizeof(msg), "Applying remote progress (%.0f%%)...", remoteBfPos.percentage);
        UITheme::drawSyncProgressPopup(renderer, "BookFusion Sync", msg);
        if (SETTINGS.darkMode) renderer.invertScreen();
        renderer.displayBuffer();
      }

      int remoteSpineIndex = 0;
      float remoteSpineProgress = 0.0f;
      if (resolveBookFusionPosition(epub, remoteBfPos, remoteSpineIndex, remoteSpineProgress)) {
        currentSpineIndex = remoteSpineIndex;
        nextPageNumber = 0;
        cachedChapterTotalPageCount = 0;
        pendingPageJump.reset();
        pendingSpineProgress = remoteSpineProgress;
        pendingPercentJump = true;
        section.reset();
        shouldUploadProgress = false;
        appliedRemoteProgress = true;
        if (remoteBfPos.updatedAt[0] != '\0') {
          BookFusionBookIdStore::saveLastSyncAt(epubPath.c_str(), remoteBfPos.updatedAt);
        } else if (internetTimeSynced) {
          // Fall back to local NTP-synced clock so the next sync has a baseline
          // to compare against and won't drop into "furthest-ahead wins".
          char localTs[40];
          if (formatLocalSyncTimestamp(localTs, sizeof(localTs))) {
            BookFusionBookIdStore::saveLastSyncAt(epubPath.c_str(), localTs);
            LOG_DBG("BFS", "Recorded local sync timestamp %s (remote omitted updated_at)", localTs);
          }
        }
        BookFusionBookIdStore::saveLastSyncedPosition(epubPath.c_str(), storedPositionFromBookFusion(remoteBfPos));
        LOG_DBG("BFS", "Applied remote BookFusion position: chapter %d, intra %.2f%%", remoteSpineIndex,
                remoteSpineProgress * 100.0f);
      } else {
        LOG_DBG("BFS", "Remote BookFusion position could not be resolved, keeping local position");
      }
    } else if (canCompareSyncState && !remoteIsNewer && !localChangedSinceLastSync) {
      shouldUploadProgress = false;
      alreadyUpToDate = true;
      LOG_DBG("BFS", "BookFusion progress already up to date");
    } else if (canCompareSyncState && remoteIsNewer && localChangedSinceLastSync && internetTimeSynced) {
      LOG_DBG("BFS", "Local position changed; using freshly synced internet time and uploading local progress");
    } else if (canCompareSyncState && remoteIsNewer && localChangedSinceLastSync) {
      LOG_DBG("BFS", "Local and remote both changed without internet time; keeping furthest-ahead fallback");
    } else if (localIsFurtherAhead && remoteIsNewer) {
      LOG_DBG("BFS", "Remote updated_at is newer but remote position is behind local; uploading local");
    }
  } else if (downloadResult == BookFusionSyncClient::NOT_FOUND) {
    LOG_DBG("BFS", "No remote progress found, will upload current position");
  } else {
    LOG_DBG("BFS", "Failed to fetch remote progress: %s", BookFusionSyncClient::errorString(downloadResult));
  }

  BookFusionSyncClient::Error uploadResult = BookFusionSyncClient::OK;
  if (shouldUploadProgress) {
    // Show direction-clear popup so the user can see we're pushing TO BookFusion.
    {
      RenderLock lock(*this);
      char msg[64];
      snprintf(msg, sizeof(msg), "Uploading local progress (%.0f%%)...", localBfPos.percentage);
      UITheme::drawSyncProgressPopup(renderer, "BookFusion Sync", msg);
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }

    BookFusionPosition uploadedBfPos = localBfPos;
    uploadResult = BookFusionSyncClient::setProgress(bookId, localBfPos, &uploadedBfPos);
    if (uploadResult == BookFusionSyncClient::OK) {
      if (uploadedBfPos.updatedAt[0] != '\0') {
        BookFusionBookIdStore::saveLastSyncAt(epubPath.c_str(), uploadedBfPos.updatedAt);
      } else if (internetTimeSynced) {
        char localTs[40];
        if (formatLocalSyncTimestamp(localTs, sizeof(localTs))) {
          BookFusionBookIdStore::saveLastSyncAt(epubPath.c_str(), localTs);
          LOG_DBG("BFS", "Recorded local sync timestamp %s (upload omitted updated_at)", localTs);
        }
      }
      BookFusionStoredPosition stored = storedPositionFromBookFusion(uploadedBfPos);
      // Capture exact page coordinates so the next sync's
      // localChangedSinceLastSync check is integer-exact instead of falling
      // through to the float epsilon (which can hide a small advance).
      stored.pageNumber = currentPage;
      stored.totalPages = totalPages;
      BookFusionBookIdStore::saveLastSyncedPosition(epubPath.c_str(), stored);
    }
  }

  // Show completion popup
  {
    RenderLock lock(*this);
    if (appliedRemoteProgress) {
      char msg[80];
      snprintf(msg, sizeof(msg), "Pulled from BookFusion\n%.1f%% applied to device", remoteBfPos.percentage);
      UITheme::drawSyncProgressPopup(renderer, "BookFusion Sync", msg);
    } else if (alreadyUpToDate) {
      UITheme::drawSyncProgressPopup(renderer, "BookFusion Sync", "Progress already up to date.");
    } else if (uploadResult == BookFusionSyncClient::OK) {
      LOG_DBG("BFS", "Progress uploaded successfully");
      char msg[80];
      snprintf(msg, sizeof(msg), "Pushed to BookFusion\n%.1f%% uploaded from device", localBfPos.percentage);
      UITheme::drawSyncProgressPopup(renderer, "BookFusion Sync", msg);
    } else {
      LOG_DBG("BFS", "Upload failed: %s", BookFusionSyncClient::errorString(uploadResult));
      char errorMsg[128];
      snprintf(errorMsg, sizeof(errorMsg), "Sync failed:\n%s", BookFusionSyncClient::errorString(uploadResult));
      UITheme::drawSyncProgressPopup(renderer, "BookFusion Sync", errorMsg);
    }
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
  }

  // Wait briefly to show the completion message, then redraw the page so
  // the user can continue reading.
  vTaskDelay(1500 / portTICK_PERIOD_MS);

  // Drop WiFi before redrawing. TLS/HTTP buffers and the LWIP control blocks
  // hold tens of KB of heap; if we leave them up the grayscale render's
  // storeBwBuffer() (six 8 KB chunks) can fail to find a contiguous block,
  // leaving the screen in a half-grayscale state — that's the post-sync
  // ghosting we saw in the field.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Force a full refresh on the next render to clear residue from the popup
  // sequence (4× fast refresh in the sync flow above) and any grayscale
  // half-frame.
  pagesUntilFullRefresh = 1;
  requestUpdateAndWait();
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
