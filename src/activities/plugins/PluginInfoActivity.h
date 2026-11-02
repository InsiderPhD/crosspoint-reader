#pragma once

#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <string>
#include <vector>

#include "PluginCatalogActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Paged text screen for the plugin flows. Two uses:
 *
 * - Per-plugin info: title, description, and the plugin's README.md as
 *   scrollable usage instructions. Pushed from the Plugins picker for a
 *   plugin with nothing to open on the device (browser-only, or an
 *   events-only device.json), so its README is how the user learns to run it.
 * - A prompt with an action: a catalog list's notice (Continue / Go Back) or a
 *   catalog item's details (Install / Fetch). Confirm finishes with
 *   isCancelled = false; Back finishes cancelled.
 *
 * Drawn with the firmware's own header / button hints (UITheme), text in the
 * standard UI font, paged a screen at a time by the side keys or a swipe.
 */
class PluginInfoActivity final : public Activity {
 public:
  explicit PluginInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, PluginRef plugin)
      : Activity("PluginInfo", renderer, mappedInput), plugin(std::move(plugin)) {}

  // Prompt form: `text` is plain text ('\n' separates paragraphs). An empty
  // `confirmLabel` makes it read-only; `backLabel` defaults to Back.
  PluginInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, std::string text,
                     std::string confirmLabel, std::string backLabel = "")
      : Activity("PluginInfo", renderer, mappedInput),
        promptText(std::move(text)),
        confirmLabel(std::move(confirmLabel)),
        backLabel(std::move(backLabel)) {
    plugin.title = std::move(title);
  }

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  PluginRef plugin;
  std::string promptText;    // prompt form only
  std::string confirmLabel;  // "" = no action
  std::string backLabel;     // "" = tr(STR_BACK)
  bool isPrompt() const { return plugin.name.empty(); }
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
  bool confirmArmed = false;  // false until the opening Confirm press is released

  void loadParagraphs();  // read raw text (no measuring) — safe in onEnter
  void ensureWrapped();   // wrap paragraphs at the current width — call on the render task
  void finishWith(bool cancelled);
};

#endif  // CROSSPOINT_SD_PLUGINS
