#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/SupportInfo.h"

struct Rect;

// Settings > System > About: a read-only list of device diagnostics (firmware,
// board, chip, memory, SD card, battery, restart reason...). The first row
// saves a privacy-safe support file to the SD card; Confirm triggers it from
// any row, since no other row does anything.
//
// The snapshot is a ~90 byte member taken once in onEnter(), so the screen
// allocates nothing of its own beyond the transient row strings drawList asks
// for while it draws.
class AboutActivity final : public Activity {
 public:
  explicit AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("About", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  enum class ExportState : uint8_t { Idle, Saved, Failed };

  ButtonNavigator buttonNavigator;
  SupportInfo::Snapshot snapshot;
  int selectedIndex = 0;
  ExportState exportState = ExportState::Idle;

  void exportSupportInfo();
  std::string valueText(int row) const;
  Rect listRect() const;
};
