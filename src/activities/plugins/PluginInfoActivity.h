#pragma once

#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <string>
#include <vector>

#include "PluginCatalogActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Per-plugin detail screen: title, description, and the plugin's README.md as
 * scrollable usage instructions. Reached from the Plugins picker for a
 * browser-only plugin (no device.json): those have nothing to open on the
 * device, so their README is how the user learns to run them from the web
 * interface. Catalog plugins open their browser directly from the picker.
 *
 * Drawn with the firmware's own header / button hints (UITheme), text in the
 * standard UI font, paged a screen at a time by the side keys or a swipe.
 */
class PluginInfoActivity final : public Activity {
 public:
  explicit PluginInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, PluginRef plugin)
      : Activity("PluginInfo", renderer, mappedInput), plugin(std::move(plugin)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  PluginRef plugin;
  ButtonNavigator buttonNavigator;
  // Raw text lines (description + README), unwrapped. Read in onEnter from
  // the SD card; bounded by MAX_README_SIZE in the .cpp.
  std::vector<std::string> paragraphs;
  // `paragraphs` word-wrapped to wrappedWidth; rebuilt on the render task
  // whenever the width changes (orientation), never in onEnter where font
  // metrics may not be final.
  std::vector<std::string> lines;
  int wrappedWidth = -1;  // width `lines` was wrapped at; -1 = not yet wrapped
  int topLine = 0;
  // Lines the last-painted screen held; measured in render() (the content
  // band and fonts are final there), read by loop()'s scroll clamp.
  int visibleRows = 1;

  void loadParagraphs();  // read raw text (no measuring) — safe in onEnter
  void ensureWrapped();   // wrap paragraphs at the current width — call on the render task
};

#endif  // CROSSPOINT_SD_PLUGINS
