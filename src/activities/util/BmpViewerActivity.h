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
  // The boot screen is read as a BMP only (BootActivity).
  bool canSetBootImage() const;
  // What Confirm does for this image: nothing (PNG), Set Cover directly (JPEG,
  // its only action) or the action list (BMP: sleep image and boot screen).
  const char* confirmLabel() const;
  void openImageActions();
  // Header of the open action list; ListPickerActivity keeps only the pointer.
  std::string actionsTitle;
  // The list closes on a button PRESS and this screen acts on the RELEASE, so
  // the key that closed it is still down when control comes back.
  bool swallowConfirmRelease = false;
  bool swallowBackRelease = false;
  // JPEG/PNG: streamed decode straight into the framebuffer, scaled to fit.
  bool renderDecodedImage();
  void showLoadError(const char* message);

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};