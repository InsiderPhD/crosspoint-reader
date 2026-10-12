#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "MappedInputManager.h"

// Full-screen image viewer for the file browser: BMP, JPEG and PNG. (The name
// predates the other two formats.)
class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void loadSiblingImages();
  void doSetSleepCover();
  // BMP is copied as-is and JPEG is converted; PNG has no path to a sleep cover.
  bool canSetSleepCover() const;
  bool saveJpegSleepCover() const;
  // JPEG/PNG: streamed decode straight into the framebuffer, scaled to fit.
  bool renderDecodedImage();
  void showLoadError(const char* message);

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};