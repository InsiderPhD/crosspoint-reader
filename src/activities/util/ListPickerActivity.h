#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * Generic single-choice list: a title, N rows named by a callback, a cursor.
 * Confirm (or a tap on the highlighted row) returns the row as a
 * ListPickResult; Back returns a cancelled result. Owns no persistence.
 *
 * An optional onCursor callback fires on every cursor move so a caller can
 * preview the choice live (the warmth picker lights the panel with it). The
 * caller is responsible for undoing the preview when the result comes back.
 */
class ListPickerActivity final : public Activity {
 public:
  using LabelFn = std::function<std::string(int index)>;
  using CursorFn = std::function<void(int index)>;

  ListPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* title, int itemCount,
                     int initialIndex, LabelFn labelFn, CursorFn onCursor = nullptr)
      : Activity("ListPicker", renderer, mappedInput),
        title(title),
        itemCount(itemCount),
        selectedIndex(initialIndex),
        labelFn(std::move(labelFn)),
        onCursor(std::move(onCursor)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  void moveTo(int index);
  void confirmSelection();
  void cancel();
  Rect listRect() const;

  const char* title;
  const int itemCount;
  int selectedIndex;
  int startIndex = 0;  // the row the picker opened on, marked in the list
  LabelFn labelFn;
  CursorFn onCursor;
  ButtonNavigator buttonNavigator;
};
