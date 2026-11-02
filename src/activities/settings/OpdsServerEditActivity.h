#pragma once

#include <I18n.h>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * One OPDS catalog's fields: name, URL, login, custom headers, and Delete.
 *
 * Edits go to a local copy and are written once, when the screen closes and
 * only if something changed (one opds.json write per visit). A new catalog is
 * added only once it has a URL; backing out of an empty one adds nothing.
 */
class OpdsServerEditActivity final : public Activity {
 public:
  // index: the catalog to edit, or -1 for a new one.
  OpdsServerEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int index);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  enum Row : int { ROW_NAME, ROW_URL, ROW_USERNAME, ROW_PASSWORD, ROW_HEADER_1, ROW_HEADER_2, ROW_DELETE };

  int rowCount() const { return index < 0 ? ROW_DELETE : ROW_DELETE + 1; }
  void handleSelection();
  void editText(StrId title, std::string& field, size_t maxLength, InputType inputType);
  void confirmDelete();
  void saveAndFinish();
  std::string rowValue(int row) const;
  Rect listRect() const;

  int index;
  OpdsServer draft;
  bool dirty = false;
  int selectedRow = 0;
  ButtonNavigator buttonNavigator;
};
