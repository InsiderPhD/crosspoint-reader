#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Txt.h>
#include <Xtc.h>

#include <string>
#include <string_view>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SleepStatsCard.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool renderSeamless =
      SETTINGS.seamlessSleepScreen == CrossPointSettings::SEAMLESS_SLEEP_SCREEN::SEAMLESS_ALWAYS ||
      (fromTimeout &&
       SETTINGS.seamlessSleepScreen == CrossPointSettings::SEAMLESS_SLEEP_SCREEN::SEAMLESS_AFTER_TIMEOUT);

  if (renderSeamless) {
    return renderLastScreenSleepScreen();
  }

  // Scrub the panel before the sleep image goes up: promote the popup paint
  // below to a full GC flash so residue accumulated while reading (fast/partial
  // waveforms are not DC-balanced) doesn't ghost through the sleep screen.
  // The seamless path above deliberately skips this — it must not flash.
  display.requestResync(1);

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

namespace {
constexpr char ROOT_SLEEP_IMAGE[] = "/sleep.bmp";
constexpr char DEFAULT_SLEEP_DIR_HIDDEN[] = "/.sleep";
constexpr char DEFAULT_SLEEP_DIR[] = "/sleep";

// Non-zero 16-bit FNV-1a fold of a folder path: tags which folder the
// recently-shown history in APP_STATE was counted in.
uint16_t sleepFolderHash(const std::string_view path) {
  uint32_t hash = 2166136261u;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  const auto folded = static_cast<uint16_t>((hash >> 16) ^ (hash & 0xFFFFu));
  return folded == 0 ? 1 : folded;
}

bool openSleepDir(const char* path, FsFile& dir) {
  dir = Storage.open(path);
  if (dir && dir.isDirectory()) {
    return true;
  }
  if (dir) dir.close();
  return false;
}

// Picks one .bmp from the open directory `dir` (whose path is `dirPath`) and
// returns its full path in `outPath`. Records the pick in APP_STATE's history.
//
// One pass, two candidates, no list. Holding every filename in a vector and
// opening every BMP to parse its header made sleep entry scale with the
// folder (two card seeks per file, plus one heap block per name). Instead:
//  - nonRecentName: a uniform reservoir sample over the images that are not
//    in the recently-shown history;
//  - oldestRecentName: of the images that are in it, the one shown longest
//    ago. Used only when every image is recent (a folder smaller than the
//    history), where it gives a strict rotation rather than a repeat.
// The caller validates the pick's header once, after selection.
bool pickSleepImage(FsFile& dir, const std::string& dirPath, std::string& outPath) {
  // The history is a list of positions in ONE folder's listing. Against any
  // other folder its indices would exclude arbitrary images, so start over.
  const uint16_t folderHash = sleepFolderHash(dirPath);
  if (APP_STATE.recentSleepFolderHash != 0 && APP_STATE.recentSleepFolderHash != folderHash) {
    APP_STATE.clearRecentSleepHistory();
  }
  APP_STATE.recentSleepFolderHash = folderHash;

  std::string nonRecentName;
  std::string oldestRecentName;
  uint16_t candidateCount = 0;
  uint16_t nonRecentCount = 0;
  uint16_t nonRecentIndex = 0;
  uint16_t oldestRecentIndex = 0;
  uint8_t oldestRecentAge = 0;
  bool haveOldestRecent = false;
  char name[500];
  for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
    if (dirFile.isDirectory()) {
      dirFile.close();
      continue;
    }
    dirFile.getName(name, sizeof(name));
    dirFile.close();
    const std::string_view filename(name);
    if (filename.empty() || filename.front() == '.') {
      continue;
    }

    if (!FsHelpers::hasBmpExtension(filename)) {
      LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
      continue;
    }

    if (candidateCount == UINT16_MAX) {
      break;
    }
    const uint16_t candidateIndex = candidateCount++;

    const uint8_t age = APP_STATE.recentSleepAge(candidateIndex);
    if (age == CrossPointState::SLEEP_NOT_RECENT) {
      nonRecentCount++;
      // Reservoir sampling: the k-th eligible image replaces the pick with
      // probability 1/k, which leaves every eligible image equally likely.
      if (random(nonRecentCount) == 0) {
        nonRecentName.assign(filename.data(), filename.size());
        nonRecentIndex = candidateIndex;
      }
    } else if (!haveOldestRecent || age > oldestRecentAge) {
      oldestRecentName.assign(filename.data(), filename.size());
      oldestRecentIndex = candidateIndex;
      oldestRecentAge = age;
      haveOldestRecent = true;
    }
  }
  if (candidateCount == 0) {
    return false;
  }

  const bool useNonRecent = nonRecentCount > 0;
  outPath = dirPath;
  if (outPath.empty() || outPath.back() != '/') outPath += '/';
  outPath += useNonRecent ? nonRecentName : oldestRecentName;
  APP_STATE.pushRecentSleep(useNonRecent ? nonRecentIndex : oldestRecentIndex);
  APP_STATE.saveToFile();
  LOG_DBG("SLP", "Randomly loading: %s (%u candidates)", outPath.c_str(), candidateCount);
  return true;
}
}  // namespace

bool SleepActivity::renderSleepImageFromFolder(const char* dirPath) const {
  FsFile dir;
  if (!openSleepDir(dirPath, dir)) {
    return false;
  }
  std::string imagePath;
  const bool picked = pickSleepImage(dir, dirPath, imagePath);
  dir.close();
  if (!picked) {
    return false;
  }

  FsFile imageFile;
  if (!Storage.openFileForRead("SLP", imagePath, imageFile)) {
    return false;
  }
  Bitmap bitmap(imageFile, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Invalid BMP sleep image: %s", imagePath.c_str());
    imageFile.close();
    return false;
  }
  renderBitmapSleepScreen(bitmap);
  imageFile.close();
  return true;
}

void SleepActivity::renderCustomSleepScreen() const {
  // A folder the user picked in the file browser wins over everything else.
  // Missing, or holding no images: the built-in lookup below still applies, so
  // a removed folder cannot leave the device without its sleep image.
  const std::string& preferredDir = APP_STATE.preferredSleepFolderPath;
  if (!preferredDir.empty()) {
    if (renderSleepImageFromFolder(preferredDir.c_str())) {
      return;
    }
    LOG_INF("SLP", "No usable image in chosen sleep folder, falling back: %s", preferredDir.c_str());
  }

  // /sleep.bmp on the root of the SD card takes priority over the default
  // sleep folders.
  FsFile file;
  if (Storage.openFileForRead("SLP", ROOT_SLEEP_IMAGE, file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: %s", ROOT_SLEEP_IMAGE);
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  // /.sleep (preferred) or /sleep. Only the first that exists is used, as
  // before: an empty /.sleep does not fall through to /sleep.
  const char* defaultDir = nullptr;
  {
    FsFile probe;
    if (openSleepDir(DEFAULT_SLEEP_DIR_HIDDEN, probe)) {
      defaultDir = DEFAULT_SLEEP_DIR_HIDDEN;
    } else if (openSleepDir(DEFAULT_SLEEP_DIR, probe)) {
      defaultDir = DEFAULT_SLEEP_DIR;
    }
    if (probe) probe.close();
  }
  if (defaultDir && renderSleepImageFromFolder(defaultDir)) {
    return;
  }

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
  SleepStatsCard::draw(renderer);

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

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  if (hasGreyscale) {
    // The grayscale LUT only nudges particles, so the BW base under it must
    // not be a strong waveform. A HALF base no longer works: HalDisplay arms a
    // resync on every HALF, which on the X3 is a full-sync waveform that sets
    // the pixels too firmly for the grey drive to move, and the cover came out
    // in harsh, wrong tones. Scrub to white on a FULL first so the base paint
    // below (a differential) doesn't ghost the reader page underneath.
    renderer.clearScreen();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // Draw the stat box into this (BW) paint. For a BW cover this is the final
  // commit. For a greyscale cover it is the base; the greyscale passes below
  // clear the box rectangle so displayGrayBuffer leaves the box region as this
  // BW base while the rest of the cover renders in greyscale.
  SleepStatsCard::draw(renderer);

  if (!hasGreyscale) {
    // Final paint: FULL_REFRESH so the image is fully committed and survives
    // the battery-disconnect power collapse (see renderDefaultSleepScreen).
    renderer.displayBuffer(HalDisplay::FULL_REFRESH, /*powerOffAfter=*/true);
    return;
  }

  // displayGrayBuffer below powers the rails off.
  //
  // The X3's UC8253 has a dedicated grey-base waveform (the OEM "AA-pre-BW(mid)"
  // bank): a differential update that gives the cover's changed pixels the
  // calibrated strong drives the gc nudge bank was tuned against. A plain FAST
  // base is the short "turbo" bank, whose blacks are too weak to start from,
  // and the grey nudge then lightens them further: every cover came out washed
  // out. The other panels have no such waveform and their base stays the FAST
  // paint they have always had.
  if (!gpio.deviceIsX3() ||
      !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Overlay, HalDisplay::FAST_REFRESH)) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }

  bitmap.rewindToData();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
  SleepStatsCard::draw(renderer, /*clearRegionOnly=*/true);  // clear box rect -> preserves BW base box
  renderer.copyGrayscaleLsbBuffers();

  bitmap.rewindToData();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
  SleepStatsCard::draw(renderer, /*clearRegionOnly=*/true);
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
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
    // Skip loading css since we only need metadata here. The framebuffer is lent to
    // the load's inflate as its 32KB dictionary (heap may have no contiguous block
    // left this deep into a session); released before any drawing below.
    bool lastEpubLoaded;
    {
      InflateScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
      lastEpubLoaded = lastEpub.load(true, true);
    }
    if (!lastEpubLoaded) {
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
