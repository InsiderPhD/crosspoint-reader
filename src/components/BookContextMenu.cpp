#include "BookContextMenu.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

bool BookContextMenu::checkLongPress(const MappedInputManager& input, std::string path, std::string title,
                                     std::string author, int progressPercent, uint32_t holdMs) {
  if (showing_) return false;
  if (triggered_) return false;
  if (!input.isPressed(MappedInputManager::Button::Confirm)) return false;
  if (input.getHeldTime() < holdMs) return false;

  triggered_ = true;
  showing_ = true;
  awaitingRelease_ = true;
  selectedIndex_ = 0;
  path_ = std::move(path);
  title_ = std::move(title);
  author_ = std::move(author);
  progressPercent_ = progressPercent;
  return true;
}

bool BookContextMenu::handleInput(ButtonNavigator& nav, const MappedInputManager& input, Action* outAction,
                                  bool* outCancelled) {
  if (!showing_) return false;

  // Allow next/prev wraparound across the option list while modal is up.
  nav.onNext([this] { selectedIndex_ = (selectedIndex_ + 1) % OPTIONS_COUNT; });
  nav.onPrevious([this] { selectedIndex_ = (selectedIndex_ - 1 + OPTIONS_COUNT) % OPTIONS_COUNT; });

  // Block selecting an option until the initial long-press hold is released.
  if (awaitingRelease_) {
    if (!input.isPressed(MappedInputManager::Button::Confirm)) {
      awaitingRelease_ = false;
    }
    return false;
  }

  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (outAction) *outAction = static_cast<Action>(selectedIndex_);
    if (outCancelled) *outCancelled = false;
    // Set the suppression flag so the host's normal short-press handler skips
    // this release (it was the menu confirmation, not "open book").
    longPressFlagSet_ = true;
    showing_ = false;
    triggered_ = false;
    return true;
  }

  if (input.wasReleased(MappedInputManager::Button::Back)) {
    if (outCancelled) *outCancelled = true;
    longPressFlagSet_ = true;  // Same reasoning — suppress short-press handler.
    showing_ = false;
    triggered_ = false;
    return true;
  }

  return false;
}

void BookContextMenu::render(GfxRenderer& renderer) const {
  if (!showing_) return;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int POPUP_W = 420;
  constexpr int BORDER = 2;
  constexpr int H_PAD = 14;
  constexpr int INFO_H = 28;
  constexpr int INFO_COUNT = 3;  // author + folder + progress
  constexpr int OPTION_H = 34;
  constexpr int LINE_V_PAD = 6;

  const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, title_.c_str(), POPUP_W - H_PAD * 2, 2);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int TITLE_H = LINE_V_PAD + static_cast<int>(titleLines.size()) * lineH + LINE_V_PAD;

  const int POPUP_H = TITLE_H + INFO_COUNT * INFO_H + OPTIONS_COUNT * OPTION_H + H_PAD;
  const int px = (pageWidth - POPUP_W) / 2;
  const int py = (pageHeight - POPUP_H) / 2;

  renderer.fillRect(px, py, POPUP_W, POPUP_H, false);
  renderer.drawRect(px, py, POPUP_W, POPUP_H, BORDER, true);

  for (int i = 0; i < static_cast<int>(titleLines.size()); i++) {
    renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + LINE_V_PAD + i * lineH, titleLines[i].c_str(), true);
  }
  renderer.drawLine(px + BORDER, py + TITLE_H, px + POPUP_W - BORDER - 1, py + TITLE_H);

  char infoBuf[80];
  snprintf(infoBuf, sizeof(infoBuf), "%s: %s", tr(STR_BOOK_INFO_AUTHOR), author_.empty() ? "--" : author_.c_str());
  renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + TITLE_H + (INFO_H - lineH) / 2, infoBuf);

  const auto lastSlash = path_.rfind('/');
  const std::string folderPath = (lastSlash != std::string::npos) ? path_.substr(0, lastSlash) : path_;
  const std::string truncatedFolder = renderer.truncatedText(UI_10_FONT_ID, folderPath.c_str(), POPUP_W - H_PAD * 2);
  renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + TITLE_H + INFO_H + (INFO_H - lineH) / 2, truncatedFolder.c_str());

  if (progressPercent_ < 0) {
    snprintf(infoBuf, sizeof(infoBuf), "%s: %s", tr(STR_BOOK_INFO_PROGRESS), tr(STR_BOOK_INFO_NOT_STARTED));
  } else {
    snprintf(infoBuf, sizeof(infoBuf), "%s: %d%%", tr(STR_BOOK_INFO_PROGRESS), progressPercent_);
  }
  renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + TITLE_H + INFO_H * 2 + (INFO_H - lineH) / 2, infoBuf);

  renderer.drawLine(px + BORDER, py + TITLE_H + INFO_COUNT * INFO_H, px + POPUP_W - BORDER - 1,
                    py + TITLE_H + INFO_COUNT * INFO_H);

  const char* options[OPTIONS_COUNT] = {tr(STR_MARK_AS_READ),       tr(STR_RESET_PROGRESS),
                                        tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE_FROM_DEVICE),
                                        tr(STR_DELETE_CACHE),        tr(STR_REGENERATE_COVER)};
  for (int i = 0; i < OPTIONS_COUNT; i++) {
    const int optY = py + TITLE_H + INFO_COUNT * INFO_H + i * OPTION_H;
    if (i == selectedIndex_) {
      renderer.fillRect(px + BORDER, optY, POPUP_W - BORDER * 2, OPTION_H, true);
    }
    renderer.drawText(UI_10_FONT_ID, px + H_PAD * 2, optY + (OPTION_H - lineH) / 2, options[i], i != selectedIndex_);
  }
}

void BookContextMenu::close() {
  showing_ = false;
  triggered_ = false;
  awaitingRelease_ = false;
  selectedIndex_ = 0;
}

bool BookContextMenu::consumeLongPressFlag() {
  const bool was = longPressFlagSet_;
  longPressFlagSet_ = false;
  return was;
}
