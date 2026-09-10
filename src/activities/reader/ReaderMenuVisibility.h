#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "EpubReaderMenuActivity.h"

// Which rows the reader menu is allowed to show. The user picks per row in
// Settings > Reader > Customise Reader Menu (ReaderMenuSettingsActivity); the
// choices live in one bitmask, SETTINGS.readerMenuVisible.
//
// Visibility is only ever a veto. A row that is switched on here still has to
// clear the menu's own conditions (a dictionary configured, a bonded remote, a
// sync backend linked to this book) before it appears.
namespace ReaderMenuVisibility {

using MenuAction = EpubReaderMenuActivity::MenuAction;

// Whether a row can appear on THIS device/build at all, regardless of what the
// user chose. Rows that fail this are left out of the customise list too --
// there is nothing to decide about a frontlight the board does not have.
enum class Availability : uint8_t {
  Always,
  DevMode,     // Dev Mode only (Screenshot, Delete Book Cache)
  Frontlight,  // board has a frontlight
  Warmth,      // board's frontlight has a warm/cool channel
};

struct Row {
  // Bit position in SETTINGS.readerMenuVisible. PERSISTED -- append new rows
  // with the next free bit and never renumber an existing one. Reordering the
  // table itself is safe: it only changes the order of the customise list.
  uint8_t bit;
  MenuAction action;
  StrId label;
  Availability availability;
};

// Listed in the order the reader menu builds them, so the customise screen
// reads like the menu it configures.
inline constexpr Row kRows[] = {
    {0, MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER, Availability::Always},
    {1, MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES, Availability::Always},
    {2, MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION, Availability::Always},
    {3, MenuAction::BUTTON_HINTS, StrId::STR_SHOW_BUTTON_HINTS, Availability::Always},
    {4, MenuAction::DARK_MODE, StrId::STR_READER_DARK_MODE, Availability::Always},
    {5, MenuAction::FRONTLIGHT_BRIGHTNESS, StrId::STR_FRONTLIGHT_BRIGHTNESS, Availability::Frontlight},
    {6, MenuAction::FRONTLIGHT_WARMTH, StrId::STR_FRONTLIGHT_WARMTH, Availability::Warmth},
    {7, MenuAction::FONT_LAYOUT, StrId::STR_FONT_LAYOUT_PREVIEW, Availability::Always},
    {8, MenuAction::READER_CONTROLS, StrId::STR_READER_CONTROLS, Availability::Always},
    {9, MenuAction::CUSTOMISE_MENU, StrId::STR_CUSTOMISE_READER_MENU, Availability::Always},
    {10, MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN, Availability::Always},
    {11, MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT, Availability::Always},
    {12, MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS, Availability::Always},
    {13, MenuAction::ADD_BOOKMARK, StrId::STR_CREATE_BOOKMARK, Availability::Always},
    {14, MenuAction::SAVE_CLIPPING, StrId::STR_SAVE_CLIPPING, Availability::Always},
    {15, MenuAction::VIEW_CLIPPINGS, StrId::STR_VIEW_CLIPPINGS, Availability::Always},
    {16, MenuAction::LOOK_UP, StrId::STR_LOOKUP, Availability::Always},
    {17, MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON, Availability::DevMode},
    {18, MenuAction::TOGGLE_BLUETOOTH, StrId::STR_BT_REMOTE_TOGGLE, Availability::Always},
    {19, MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR, Availability::Always},
    {20, MenuAction::NEARBY_POSITION_SYNC, StrId::STR_NEARBY_POSITION_SYNC, Availability::Always},
    {21, MenuAction::AUTOSYNC, StrId::STR_AUTOSYNC, Availability::Always},
    {22, MenuAction::SYNC_PUSH, StrId::STR_SYNC_PUSH, Availability::Always},
    {23, MenuAction::SYNC_PULL, StrId::STR_SYNC_PULL, Availability::Always},
    {24, MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE, Availability::DevMode},
    {25, MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON, Availability::Always},
};

inline constexpr size_t kRowCount = std::size(kRows);

// True when this device/build could ever show the row (see Availability).
bool isAvailable(const Row& row);

// The user's choice for a row. Unknown actions read as visible so a row that
// somehow escapes the table can never disappear silently.
bool isVisible(MenuAction action);

void setVisible(MenuAction action, bool visible);

// Clears every hide, including bits belonging to rows this build doesn't know.
void showAll();

// One-time migration for settings files written before the per-row bitmask,
// which carried only four coarse group toggles. Anything those didn't cover
// stays visible. Keeps the bit numbers in this file, where kRows can be checked
// against them.
uint32_t maskFromLegacyToggles(bool bookmarks, bool clippings, bool bluetooth, bool sync);

}  // namespace ReaderMenuVisibility
