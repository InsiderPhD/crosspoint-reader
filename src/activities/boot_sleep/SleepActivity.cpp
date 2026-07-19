#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ReadingStatsStore.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool renderSeamless =
      SETTINGS.seamlessSleepScreen == CrossPointSettings::SEAMLESS_SLEEP_SCREEN::SEAMLESS_ALWAYS ||
      (fromTimeout &&
       SETTINGS.seamlessSleepScreen == CrossPointSettings::SEAMLESS_SLEEP_SCREEN::SEAMLESS_AFTER_TIMEOUT);

  if (renderSeamless) {
    return renderLastScreenSleepScreen();
  }

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.sleepScreenOrientation);

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      if (dir) dir.close();
      return;
    }
    file.close();
  }

  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
      if (dirFile.isDirectory()) {
        dirFile.close();
        continue;
      }
      dirFile.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        dirFile.close();
        continue;
      }

      if (!FsHelpers::hasBmpExtension(filename)) {
        LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
        dirFile.close();
        continue;
      }
      Bitmap bitmap(dirFile);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
        dirFile.close();
        continue;
      }
      files.emplace_back(filename);
      dirFile.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Pick a random wallpaper, excluding recently shown ones.
      // Window: up to SLEEP_RECENT_COUNT entries, capped at numFiles-1.
      const uint16_t fileCount = static_cast<uint16_t>(std::min(numFiles, static_cast<size_t>(UINT16_MAX)));
      const uint8_t window =
          static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), numFiles - 1));
      auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
      for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
        randomFileIndex = static_cast<uint16_t>(random(fileCount));
      }
      APP_STATE.pushRecentSleep(randomFileIndex);
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      FsFile randFile;
      if (Storage.openFileForRead("SLP", filename, randFile)) {
        LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(randFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          randFile.close();
          dir.close();
          return;
        }
        randFile.close();
      }
    }
  }
  if (dir) dir.close();

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Draw the stat cards before any invertScreen() below so they invert with the
  // rest of the dark-mode screen.
  drawStatLines();

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  // Terminate on a FULL_REFRESH, not a partial one. On battery, deep sleep
  // opens the battery-protection MOSFET (HalPowerManager::startDeepSleep) and
  // the panel rail collapses entirely. A partial waveform (HALF/FAST) leaves
  // the e-ink particles in a metastable state that relies on a powered
  // controller to hold it; once VCC drops, the particles relax and the old
  // image ghosts back over a few seconds. A full waveform commits every pixel
  // to its bistable rail so the image survives power removal. powerOffAfter
  // still collapses the rails cleanly as the terminating phase of the refresh.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH, /*powerOffAfter=*/true);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // Draw the stat box into this (BW) paint. For a BW cover this is the final
  // commit. For a greyscale cover this HALF paint becomes the base image that
  // displayGrayBuffer() refines; the greyscale passes below then clear the box
  // rectangle so displayGrayBuffer leaves the box region as this BW base while
  // the rest of the cover renders in greyscale — one refresh, greyscale kept.
  drawStatLines();

  // Power off after this paint only when it is the final one. The greyscale
  // pass below repaints and powers down on its own (displayGrayBuffer always
  // collapses the rails after its update). When this IS the final paint (BW
  // cover, no greyscale), use FULL_REFRESH so the image is fully committed and
  // survives the battery-disconnect power collapse (see renderDefaultSleepScreen
  // for the full rationale). The intermediate greyscale paint stays on
  // HALF_REFRESH since displayGrayBuffer supersedes it.
  renderer.displayBuffer(hasGreyscale ? HalDisplay::HALF_REFRESH : HalDisplay::FULL_REFRESH,
                         /*powerOffAfter=*/!hasGreyscale);

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    drawStatLines(/*clearRegionOnly=*/true);  // clear box rect -> preserves BW base box
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    drawStatLines(/*clearRegionOnly=*/true);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  LOG_DBG("SLP", "renderCoverSleepScreen: openEpubPath='%s'", APP_STATE.openEpubPath.c_str());

  if (APP_STATE.openEpubPath.empty()) {
    LOG_DBG("SLP", "Falling back: openEpubPath is empty");
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    LOG_DBG("SLP", "EPUB path; loading metadata + ensuring cover bmp (cropped=%d)", cropped);
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
    LOG_DBG("SLP", "Cover bmp ready at: %s", coverBmpPath.c_str());
  } else {
    LOG_DBG("SLP", "Falling back: unsupported file extension for %s", APP_STATE.openEpubPath.c_str());
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (!Storage.openFileForRead("SLP", coverBmpPath, file)) {
    LOG_ERR("SLP", "Falling back: could not open cover bmp for reading: %s", coverBmpPath.c_str());
    return (this->*renderNoCoverSleepScreen)();
  }
  Bitmap bitmap(file);
  const auto parseErr = bitmap.parseHeaders();
  if (parseErr != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Falling back: cover bmp parseHeaders failed (err=%d) for: %s", static_cast<int>(parseErr),
            coverBmpPath.c_str());
    return (this->*renderNoCoverSleepScreen)();
  }
  LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
  renderBitmapSleepScreen(bitmap);
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  // Deliberately stays on HALF_REFRESH (partial waveform): the whole point of
  // seamless sleep is to keep the last page on screen WITHOUT a flash, and a
  // FULL_REFRESH inverts the panel. The tradeoff is that on battery the
  // metastable pixels can ghost slightly as the rail collapses (see
  // renderDefaultSleepScreen). Seamless users opt into flash-free over
  // ghost-free; do not "fix" this to FULL_REFRESH.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, /*powerOffAfter=*/true);
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  // FULL_REFRESH so the blank field is fully committed and does not let the
  // prior image ghost back when the battery rail collapses (see
  // renderDefaultSleepScreen for the full rationale).
  renderer.displayBuffer(HalDisplay::FULL_REFRESH, /*powerOffAfter=*/true);
}

// Day of week for a Gregorian date via Sakamoto's algorithm. 0 = Sunday .. 6 = Saturday.
static int dayOfWeek(int y, unsigned m, unsigned d) {
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + static_cast<int>(d)) % 7;
}

uint8_t SleepActivity::buildStatLines(StatLine lines[MAX_STAT_LINES]) const {
  const uint8_t slots[3] = {SETTINGS.sleepStatSlot1, SETTINGS.sleepStatSlot2, SETTINGS.sleepStatSlot3};
  constexpr size_t N = STAT_LINE_LEN;
  uint8_t count = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (count >= MAX_STAT_LINES) break;  // no room for more lines
    lines[count].weekDots = false;
    char* buf = lines[count].text;
    switch (slots[i]) {
      case CrossPointSettings::SLEEP_STAT_TODAY:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_TODAY),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getTodayReadingMs()).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_GOAL: {
        const uint64_t todayMs = READING_STATS.getTodayReadingMs();
        const uint64_t goalMs = SETTINGS.getDailyGoalMs();
        if (todayMs >= goalMs) {
          snprintf(buf, N, "%s", tr(STR_STAT_GOAL_MET));
        } else {
          snprintf(buf, N, "%s: %s / %s", tr(STR_STAT_GOAL), ReadingStatsAnalytics::formatDurationHm(todayMs).c_str(),
                   ReadingStatsAnalytics::formatDurationHm(goalMs).c_str());
        }
        break;
      }
      case CrossPointSettings::SLEEP_STAT_WEEK:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_WEEK),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getRecentReadingMs(7)).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_MONTH:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_MONTH),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getRecentReadingMs(30)).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_STREAK:
        snprintf(buf, N, "%s: %u %s", tr(STR_STAT_STREAK), static_cast<unsigned>(READING_STATS.getCurrentStreakDays()),
                 tr(STR_STAT_DAYS));
        break;
      case CrossPointSettings::SLEEP_STAT_TOTAL:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_TOTAL),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getTotalReadingMs()).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_FINISHED:
        snprintf(buf, N, "%s: %u", tr(STR_STAT_FINISHED), static_cast<unsigned>(READING_STATS.getBooksFinishedCount()));
        break;
      case CrossPointSettings::SLEEP_STAT_RETURN:
        if (SETTINGS.returnContact[0] == '\0') {
          continue;  // no contact entered -> nothing to show
        }
        // Two lines: the "please return to" label, then the contact on its own
        // line so a full email address has room. (The post-switch count++ below
        // accounts for the label line; this consumes the extra contact line.)
        snprintf(lines[count].text, N, "%s", tr(STR_STAT_RETURN_MSG));
        if (count + 1 < MAX_STAT_LINES) {
          lines[count + 1].weekDots = false;
          snprintf(lines[count + 1].text, N, "%s", SETTINGS.returnContact);
          count++;
        }
        break;
      case CrossPointSettings::SLEEP_STAT_BOOK_PROGRESS: {
        if (APP_STATE.openEpubPath.empty()) {
          continue;  // no book open (e.g. sleeping from home) -> nothing to show
        }
        const ReadingBookStats* book = READING_STATS.findMatchingBookForPath(APP_STATE.openEpubPath);
        if (book == nullptr) {
          continue;  // book not tracked yet -> nothing to show
        }
        snprintf(buf, N, "%s: %u%%", tr(STR_STAT_BOOK_PROGRESS), static_cast<unsigned>(book->lastProgressPercent));
        break;
      }
      case CrossPointSettings::SLEEP_STAT_BOOK_TIME_LEFT: {
        if (APP_STATE.openEpubPath.empty()) {
          continue;  // no book open
        }
        // Prefer the reader's own pages-left × pace figure, stashed at reader
        // exit — the same estimate the status bar / reader menu show, and it
        // doesn't hide after short sessions the way percent-based pace does.
        if (APP_STATE.readerTimeLeftSeconds > 0 && APP_STATE.readerTimeLeftBookPath == APP_STATE.openEpubPath) {
          const uint64_t timeLeftMs = static_cast<uint64_t>(APP_STATE.readerTimeLeftSeconds) * 1000ULL;
          snprintf(buf, N, "%s %s", ReadingStatsAnalytics::formatDurationHm(timeLeftMs).c_str(),
                   tr(STR_STAT_BOOK_TIME_LEFT_MSG));
          break;
        }
        // Fallback (no stash: pre-update state.json, non-EPUB book, or reader
        // exited at 0 pages left): pace of the just-finished session —
        // "read (end-start)% of the book in sessionMs, so (100-end)% remaining
        // takes remaining/pace". Recent-reading only, so it survives a stats
        // reset, unlike extrapolating lifetime time vs. progress.
        const ReadingSessionSnapshot& snap = READING_STATS.getLastSessionSnapshot();
        if (!snap.valid || snap.path != APP_STATE.openEpubPath || snap.sessionMs == 0 ||
            snap.endProgressPercent <= snap.startProgressPercent || snap.endProgressPercent >= 100) {
          continue;  // no recent forward progress to estimate a pace from
        }
        const uint32_t gainedPct = snap.endProgressPercent - snap.startProgressPercent;
        const uint32_t remainingPct = 100 - snap.endProgressPercent;
        const uint64_t timeLeftMs = static_cast<uint64_t>(snap.sessionMs) * remainingPct / gainedPct;
        snprintf(buf, N, "%s %s", ReadingStatsAnalytics::formatDurationHm(timeLeftMs).c_str(),
                 tr(STR_STAT_BOOK_TIME_LEFT_MSG));
        break;
      }
      case CrossPointSettings::SLEEP_STAT_GOAL_LEFT: {
        const uint64_t todayMs = READING_STATS.getTodayReadingMs();
        const uint64_t goalMs = SETTINGS.getDailyGoalMs();
        if (todayMs >= goalMs) {
          snprintf(buf, N, "%s", tr(STR_STAT_GOAL_MET));
        } else {
          snprintf(buf, N, "%s %s", ReadingStatsAnalytics::formatDurationHm(goalMs - todayMs).c_str(),
                   tr(STR_STAT_GOAL_LEFT_MSG));
        }
        break;
      }
      case CrossPointSettings::SLEEP_STAT_DAILY_AVG:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_DAILY_AVG),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getRecentReadingMs(7) / 7).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_DAYS_MONTH: {
        // Count reading days whose (year, month) matches the reference date —
        // mirrors buildMonthSummary() in ReadingStatsActivity.
        const uint32_t refOrdinal = TimeUtils::getLocalDayOrdinal(READING_STATS.getDisplayTimestamp());
        int refYear = 0;
        unsigned refMonth = 0, refDay = 0;
        if (!TimeUtils::getDateFromDayOrdinal(refOrdinal, refYear, refMonth, refDay)) {
          continue;  // clock not set -> can't scope to a month
        }
        uint32_t daysRead = 0;
        for (const auto& d : READING_STATS.getReadingDays()) {
          if (d.readingMs == 0) continue;
          int y = 0;
          unsigned m = 0, dd = 0;
          if (TimeUtils::getDateFromDayOrdinal(d.dayOrdinal, y, m, dd) && y == refYear && m == refMonth) {
            daysRead++;
          }
        }
        snprintf(buf, N, "%s: %u", tr(STR_STAT_DAYS_MONTH), static_cast<unsigned>(daysRead));
        break;
      }
      case CrossPointSettings::SLEEP_STAT_WEEK_STREAK: {
        // Mon-Sun of the current week. Bit d (0 = Monday .. 6 = Sunday) is set when
        // that weekday met the daily goal. Future days this week simply stay unset.
        const uint32_t todayOrd = TimeUtils::getLocalDayOrdinal(READING_STATS.getDisplayTimestamp());
        int ty = 0;
        unsigned tm = 0, tdd = 0;
        if (!TimeUtils::getDateFromDayOrdinal(todayOrd, ty, tm, tdd)) {
          continue;  // clock not set -> can't anchor the week
        }
        const int dow = dayOfWeek(ty, tm, tdd);  // 0 = Sunday
        const uint32_t mondayOrd = todayOrd - static_cast<uint32_t>((dow + 6) % 7);
        const uint64_t goalMs = SETTINGS.getDailyGoalMs();
        uint8_t mask = 0;
        for (uint8_t d = 0; d < 7; d++) {  // Monday..Sunday
          const uint32_t ord = mondayOrd + d;
          uint64_t ms = 0;
          for (const auto& rd : READING_STATS.getReadingDays()) {
            if (rd.dayOrdinal == ord) {
              ms = rd.readingMs;
              break;
            }
          }
          if (ms >= goalMs) mask = static_cast<uint8_t>(mask | (1u << d));
        }
        lines[count].weekDots = true;
        lines[count].weekMask = mask;
        lines[count].weekCount = 7;
        lines[count].text[0] = '\0';
        break;
      }
      default:  // SLEEP_STAT_NONE or out-of-range: skip this slot
        continue;
    }
    count++;
  }
  return count;
}

void SleepActivity::drawStatLines(bool clearRegionOnly) const {
  StatLine lines[MAX_STAT_LINES];
  const uint8_t count = buildStatLines(lines);
  if (count == 0) return;  // no stats selected -> leave the screen untouched

  // A single rounded card (home-menu style) holding all the stat lines.
  constexpr int STATS_FONT_ID = UI_12_FONT_ID;
  constexpr int LETTER_FONT_ID = SMALL_FONT_ID;  // weekday letters inside the cells
  constexpr int RADIUS = 8;
  constexpr int BORDER = 2;
  constexpr int PAD_X = 24;
  constexpr int PAD_Y = 14;
  constexpr int LINE_GAP = 6;
  constexpr int CELL_GAP = 6;  // gap between weekly-streak cells
  static constexpr char WEEK_LETTERS[7] = {'M', 'T', 'W', 'T', 'F', 'S', 'S'};

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(STATS_FONT_ID);
  const int cell = lineH;  // weekly-streak cell = one line tall
  auto weekRowWidth = [&](uint8_t cells) { return cells * cell + (cells - 1) * CELL_GAP; };

  // Box width sized to the widest line/row (clamped to the screen).
  int maxLineW = 0;
  for (uint8_t i = 0; i < count; i++) {
    const int w =
        lines[i].weekDots ? weekRowWidth(lines[i].weekCount) : renderer.getTextWidth(STATS_FONT_ID, lines[i].text);
    if (w > maxLineW) maxLineW = w;
  }
  int boxW = maxLineW + 2 * PAD_X;
  const int maxBoxW = pageWidth - 8;
  if (boxW > maxBoxW) boxW = maxBoxW;
  const int boxH = count * lineH + (count - 1) * LINE_GAP + 2 * PAD_Y;
  const int boxX = (pageWidth - boxW) / 2;
  // Bottom-middle: sit the box in the lower ~1/8 margin, not flush to the edge.
  const int boxY = pageHeight - pageHeight / 8 - boxH;

  // Greyscale compositing: the box is painted into the BW base image, and each
  // greyscale pass just clears the box rectangle so displayGrayBuffer leaves that
  // region untouched (keeping the BW box) while the rest renders in greyscale.
  if (clearRegionOnly) {
    renderer.fillRect(boxX, boxY, boxW, boxH, /*state=*/true);  // clear both planes -> "no change"
    return;
  }

  // White fill + black rounded border + black centred lines. Runs in BW render
  // mode; on the dark default screen the caller's invertScreen() then flips the
  // box to a white outline + white text/cells on black. Centred content lines up
  // with the screen-centred box.
  renderer.fillRoundedRect(boxX, boxY, boxW, boxH, RADIUS, Color::White);
  renderer.drawRoundedRect(boxX, boxY, boxW, boxH, BORDER, RADIUS, /*state=*/true);
  for (uint8_t i = 0; i < count; i++) {
    const int lineTop = boxY + PAD_Y + i * (lineH + LINE_GAP);
    if (lines[i].weekDots) {
      // A labelled cell per weekday: draw the letter, then fill (which hides the
      // letter) when the goal was met, or just outline it when it wasn't.
      const uint8_t cells = lines[i].weekCount;
      const int startX = (pageWidth - weekRowWidth(cells)) / 2;
      const int letterH = renderer.getLineHeight(LETTER_FONT_ID);
      for (uint8_t d = 0; d < cells; d++) {
        const int cx = startX + d * (cell + CELL_GAP);
        const char letter[2] = {WEEK_LETTERS[d], '\0'};
        const int letterW = renderer.getTextWidth(LETTER_FONT_ID, letter);
        renderer.drawText(LETTER_FONT_ID, cx + (cell - letterW) / 2, lineTop + (cell - letterH) / 2, letter,
                          /*black=*/true);
        if (lines[i].weekMask & (1u << d)) {
          renderer.fillRect(cx, lineTop, cell, cell, /*state=*/true);
        } else {
          renderer.drawRect(cx, lineTop, cell, cell, /*state=*/true);
        }
      }
    } else {
      // drawText/drawCenteredText take the text TOP (they add the ascender
      // internally), so pass the line's top edge inside the box.
      renderer.drawCenteredText(STATS_FONT_ID, lineTop, lines[i].text, /*black=*/true);
    }
  }
}
