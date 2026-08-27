#pragma once
#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Split-screen reader-settings editor with a live sample-text preview.
//
// The top region renders two sample paragraphs re-laid-out (greedy word-wrap,
// alignment, bionic split) using the current reader font/layout settings; the
// bottom region is a settings list. Changing any row mutates the SETTINGS
// singleton live and re-renders instantly, so the effect is visible before you
// return to the book. Settings persist once on exit; the reader repaginates
// automatically because the section-file cache is keyed on these values.
//
// Launched from both the in-book reader menu and Settings -> Reader.
class FontLayoutPreviewActivity final : public Activity {
 public:
  explicit FontLayoutPreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Full Touch (X4 Pro): rows are tap-dispatched here, so the main loop must
  // stop injecting Confirm for taps.
  bool handlesDirectTouch() const override { return true; }

 private:
  enum class RowKind : uint8_t { Enum, Value, Toggle, Action };

  // One editable row. For Enum/Value/Toggle, `field` points at the backing
  // CrossPointSettings member; Enum also uses `enumLabels`/`enumCount`, Value
  // uses the min/max/step range. Action rows launch a sub-activity.
  struct Row {
    StrId label;
    RowKind kind;
    uint8_t CrossPointSettings::* field;
    const StrId* enumLabels;
    uint8_t enumCount;
    uint8_t valMin;
    uint8_t valMax;
    uint8_t valStep;
    bool affectsFont;    // font size — reload SD font glyphs after change (font family uses cycleFont)
    bool isOrientation;  // needs applyOrientation() + a full refresh to avoid ghosting
  };

  static const Row kRows[];
  static const int kRowCount;

  // Orientation-aware screen split, shared by render() and the loop()'s tap
  // hit-testing so paint and hit-test can never disagree about where a row is.
  struct Layout {
    int contentX;
    int contentW;
    int contentTop;
    int previewTop;
    int previewH;
    int sepY;
    int topH;   // region above the list: title + sample + separator
    int listY;  // top of the settings list
    int listH;
  };
  Layout layout() const;
  Rect listRect() const;  // layout().list* as the rect handed to drawList/hitTestList

  std::string rowValueText(const Row& row) const;
  void changeRow(const Row& row);
  void cycleFont();            // font-family row: cycle built-ins + installed SD families
  void prewarmSampleGlyphs();  // load SD-font sample glyph bitmaps into RAM (no-op for built-ins)
  void activateRow(const Row& row);
  void activateSelectedRow();  // Confirm / second tap on the selected row
  void renderPreview(int x, int y, int w, int h) const;

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool changed = false;          // whether to persist on exit
  bool fullRefreshNext = false;  // orientation/action changes need a full refresh
  bool aaPreviewNext = false;    // show the AA grayscale pass once, right after AA is enabled
  GfxRenderer::Orientation entryOrientation = GfxRenderer::Orientation::Portrait;

  // Snapshot of the title+sample region so plain navigation (which doesn't change
  // the preview) can restore it instead of re-laying-out ~60 words every keypress.
  bool previewDirty = true;
  bool previewValid = false;
  uint8_t* previewSnap = nullptr;
  size_t previewSnapCap = 0;
  int snapW = 0;
  int snapH = 0;
};
