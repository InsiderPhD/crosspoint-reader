#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"
#include "util/DictionaryRegistry.h"

class MappedInputManager;

/**
 * Picks the StarDict dictionary used for reader lookups.
 *
 * Row 0 is always "None" (lookups disabled); the rest are the folders found
 * under /dictionaries and /.dictionaries. The chosen folder NAME is what
 * persists, so adding or removing a dictionary cannot silently repoint the
 * setting at a different one.
 */
class DictionarySelectActivity final : public Activity {
 public:
  explicit DictionarySelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictionarySelect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  void handleSelection();
  Rect listRect() const;
  int totalItems() const { return static_cast<int>(dictionaries.size()) + 1; }

  void onBack() { finish(); }

  ButtonNavigator buttonNavigator;
  // Rescanned on entry: one directory listing, and it picks up dictionaries
  // copied to the SD card since the last visit.
  std::vector<DictionaryEntry> dictionaries;
  int selectedIndex = 0;
};
