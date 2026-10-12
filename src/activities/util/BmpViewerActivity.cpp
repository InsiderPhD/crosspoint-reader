#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/JpegToFramebufferConverter.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char SLEEP_COVER_PATH[] = "/sleep.bmp";
constexpr char SLEEP_COVER_TMP_PATH[] = "/sleep.bmp.tmp";
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string fname(name);
        if (FsHelpers::hasImageExtension(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  for (size_t i = 0; i < siblingImages.size(); ++i) {
    if (siblingImages[i] == fileName) {
      currentImageIndex = static_cast<int>(i);
      break;
    }
  }
}

bool BmpViewerActivity::canSetSleepCover() const {
  return FsHelpers::hasBmpExtension(filePath) || FsHelpers::hasJpgExtension(filePath);
}

void BmpViewerActivity::showLoadError(const char* message) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, message);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool BmpViewerActivity::renderDecodedImage() {
  // Both decoders stream: JPEG in MCU bands through JPEGDEC's built-in 1/2..1/8
  // downscale, PNG a scanline at a time. Neither ever holds the decoded image,
  // so RAM does not grow with the picture; the decoder object (about 20 KB for
  // JPEG) is heap-allocated for the length of one pass and freed again.
  const bool jpeg = FsHelpers::hasJpgExtension(filePath);
  JpegToFramebufferConverter jpegDecoder;
  PngToFramebufferConverter pngDecoder;
  ImageToFramebufferDecoder& decoder =
      jpeg ? static_cast<ImageToFramebufferDecoder&>(jpegDecoder) : static_cast<ImageToFramebufferDecoder&>(pngDecoder);

  ImageDimensions dimensions{};
  if (!decoder.getDimensions(filePath, dimensions) || dimensions.width <= 0 || dimensions.height <= 0) {
    return false;
  }

  // Scale to fit the current orientation, never up, and centre.
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int width = dimensions.width;
  int height = dimensions.height;
  if (width > screenWidth || height > screenHeight) {
    if (static_cast<int32_t>(width) * screenHeight > static_cast<int32_t>(height) * screenWidth) {
      height = std::max(1, height * screenWidth / width);
      width = screenWidth;
    } else {
      width = std::max(1, width * screenHeight / height);
      height = screenHeight;
    }
  }
  RenderConfig config{(screenWidth - width) / 2, (screenHeight - height) / 2, width, height};
  config.useExactDimensions = true;

  const bool hasPrevious = siblingImages.size() > 1 && currentImageIndex > 0;
  const bool hasNext = siblingImages.size() > 1 && currentImageIndex != -1 &&
                       currentImageIndex < static_cast<int>(siblingImages.size()) - 1;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canSetSleepCover() ? tr(STR_SET_SLEEP_COVER) : "",
                                            hasPrevious ? "<" : "", hasNext ? ">" : "");
  const auto drawImage = [&]() {
    if (!decoder.decodeToFramebuffer(filePath, renderer, config)) return false;
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return true;
  };

  // The decoders emit four grey levels, of which a B/W frame keeps only "white
  // or not", so the picture needs the two grayscale planes on top of the B/W
  // base. Each plane is a fresh decode: re-reading the file costs time, where
  // keeping the pixels would cost a second frame of RAM.
  renderer.clearScreen();
  if (!drawImage()) return false;

  // Same choice of base as the reader's image pages (absolute planes where the
  // controller takes them) and the sleep cover (overlay everywhere else).
  const bool absolute = !gpio.deviceIsX3() && display.getController() == HalDisplay::Controller::UC8279 &&
                        renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  if (absolute) {
    if (!renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) {
      LOG_ERR("BMP", "Absolute grayscale base refused; showing B/W");
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return true;
    }
  } else if (!gpio.deviceIsX3() ||
             !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Overlay, HalDisplay::FAST_REFRESH)) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }

  bool planesReady = true;
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    renderer.clearScreen(absolute ? 0xFF : 0x00);
    renderer.setRenderMode(mode);
    if (!drawImage()) {
      planesReady = false;
      break;
    }
    if (mode == GfxRenderer::GRAYSCALE_LSB) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  // Commit before leaving grayscale mode: setRenderMode(BW) cancels an armed
  // absolute pass (and is what backs out of one that failed half way).
  if (planesReady) renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  // The framebuffer now holds a plane, not the picture. Redraw the B/W frame
  // and hand it to the controller so the next partial update diffs against
  // what is really on the panel. (storeBwBuffer would save this decode at the
  // price of 48 KB of heap.)
  renderer.clearScreen();
  const bool redrawn = drawImage();
  renderer.cleanupGrayscaleWithFrameBuffer();
  return planesReady && redrawn;
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  FsFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress

  if (FsHelpers::hasJpgExtension(filePath) || FsHelpers::hasPngExtension(filePath)) {
    // Not inverted in dark mode, like the BMP path below: a picture is shown in
    // its true polarity.
    if (!renderDecodedImage()) {
      LOG_ERR("BMP", "Could not show image: %s", filePath.c_str());
      showLoadError(tr(STR_FIRMWARE_FILE_OPEN_FAILED));
    }
    return;
  }

  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      // Assuming drawBitmap defaults to 0,0 crop if omitted, or pass explicitly: drawBitmap(bitmap, x, y, pageWidth,
      // pageHeight, 0, 0)
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // Single pass for non-grayscale images

      // Deliberately not inverted in dark mode: this previews the picture as it
      // will appear when set as the sleep cover, so it must show true polarity.
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Invalid BMP File");
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Could not open file");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool BmpViewerActivity::saveJpegSleepCover() const {
  // The same streamed JPEG -> 2-bit BMP conversion that makes a book's sleep
  // cover. Written to a temp file first so a failed conversion cannot leave a
  // half-written /sleep.bmp behind.
  FsFile jpegFile;
  if (!Storage.openFileForRead("BMP", filePath, jpegFile)) return false;
  FsFile bmpFile;
  if (!Storage.openFileForWrite("BMP", SLEEP_COVER_TMP_PATH, bmpFile)) {
    jpegFile.close();
    return false;
  }
  const bool crop = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  const bool converted = JpegToBmpConverter::jpegFileToBmpStream(jpegFile, bmpFile, crop);
  jpegFile.close();
  bmpFile.close();
  if (!converted) {
    LOG_ERR("BMP", "Sleep cover conversion failed: %s", filePath.c_str());
    Storage.remove(SLEEP_COVER_TMP_PATH);
    return false;
  }
  if (Storage.exists(SLEEP_COVER_PATH)) Storage.remove(SLEEP_COVER_PATH);
  if (!Storage.rename(SLEEP_COVER_TMP_PATH, SLEEP_COVER_PATH)) {
    LOG_ERR("BMP", "Failed to publish sleep cover");
    return false;
  }
  return true;
}

void BmpViewerActivity::doSetSleepCover() {
  if (!canSetSleepCover()) return;
  GUI.drawPopup(renderer, tr(STR_LOADING));

  bool success = false;
  FsFile inFile, outFile;
  if (FsHelpers::hasJpgExtension(filePath)) {
    success = saveJpegSleepCover();
  } else if (Storage.openFileForRead("BMP", filePath, inFile)) {
    if (Storage.openFileForWrite("BMP", SLEEP_COVER_PATH, outFile)) {
      char buffer[2048];
      int bytesRead;
      success = true;
      while ((bytesRead = inFile.read(buffer, sizeof(buffer))) > 0) {
        if (outFile.write(buffer, bytesRead) != bytesRead) {
          success = false;
          break;
        }
      }
      outFile.close();
    }
    inFile.close();
  }

  if (success) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSetSleepCover();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (siblingImages.size() > 1 && currentImageIndex > 0) {
      currentImageIndex--;
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (siblingImages.size() > 1 && currentImageIndex != -1 &&
        currentImageIndex < static_cast<int>(siblingImages.size()) - 1) {
      currentImageIndex++;
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }
}