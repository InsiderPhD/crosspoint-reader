#include "ReaderMenuVisibility.h"

#include <HalFrontlight.h>

#include <initializer_list>

#include "CrossPointSettings.h"

namespace ReaderMenuVisibility {

namespace {
const Row* findRow(const MenuAction action) {
  for (const auto& row : kRows) {
    if (row.action == action) return &row;
  }
  return nullptr;
}
}  // namespace

bool isAvailable(const Row& row) {
  switch (row.availability) {
    case Availability::Always:
      return true;
    case Availability::DevMode:
      return SETTINGS.devMode != 0;
    case Availability::Frontlight:
#if FREEINK_CAP_FRONTLIGHT
      return halFrontlight.present();
#else
      return false;
#endif
    case Availability::Warmth:
#if FREEINK_CAP_FRONTLIGHT
      // Same fence the menu itself uses: hasWarmth() is what separates a
      // warm/cool light from a plain one, not a second capability macro.
      return halFrontlight.present() && halFrontlight.hasWarmth();
#else
      return false;
#endif
  }
  return true;
}

bool isVisible(const MenuAction action) {
  const Row* row = findRow(action);
  if (!row) return true;
  return (SETTINGS.readerMenuVisible & (1u << row->bit)) != 0;
}

void setVisible(const MenuAction action, const bool visible) {
  const Row* row = findRow(action);
  if (!row) return;
  if (visible) {
    SETTINGS.readerMenuVisible |= (1u << row->bit);
  } else {
    SETTINGS.readerMenuVisible &= ~(1u << row->bit);
  }
}

void showAll() { SETTINGS.readerMenuVisible = CrossPointSettings::READER_MENU_VISIBLE_ALL; }

uint32_t maskFromLegacyToggles(const bool bookmarks, const bool clippings, const bool bluetooth, const bool sync) {
  uint32_t mask = CrossPointSettings::READER_MENU_VISIBLE_ALL;
  const auto hide = [&mask](const std::initializer_list<MenuAction> actions) {
    for (const MenuAction action : actions) {
      if (const Row* row = findRow(action)) mask &= ~(1u << row->bit);
    }
  };
  if (!bookmarks) hide({MenuAction::BOOKMARKS, MenuAction::ADD_BOOKMARK});
  if (!clippings) hide({MenuAction::SAVE_CLIPPING, MenuAction::VIEW_CLIPPINGS});
  if (!bluetooth) hide({MenuAction::TOGGLE_BLUETOOTH});
  if (!sync) hide({MenuAction::AUTOSYNC, MenuAction::SYNC_PUSH, MenuAction::SYNC_PULL});
  return mask;
}

}  // namespace ReaderMenuVisibility
