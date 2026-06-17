#include "BookContextMenu.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace {
using Action = BookContextMenu::Action;

// Build the list of currently-visible actions, in display order. The testing-only
// actions (Reset Progress, Delete Book Cache, Regenerate Cover) are shown only when
// Dev Mode is enabled. Returns the count; fills out[] (sized OPTIONS_COUNT).
int collectActions(Action* out) {
  const bool dev = SETTINGS.devMode != 0;
  int n = 0;
  out[n++] = Action::MarkRead;
  if (dev) out[n++] = Action::ResetProgress;
  out[n++] = Action::Shelve;
  out[n++] = Action::Delete;
  if (dev) out[n++] = Action::Reindex;
  if (dev) out[n++] = Action::RegenerateCover;
  out[n++] = Action::BookInfo;
  return n;
}

const char* labelForAction(Action action) {
  switch (action) {
    case Action::MarkRead:
      return tr(STR_MARK_AS_READ);
    case Action::ResetProgress:
      return tr(STR_RESET_PROGRESS);
    case Action::Shelve:
      return tr(STR_REMOVE_FROM_RECENTS);
    case Action::Delete:
      return tr(STR_DELETE_FROM_DEVICE);
    case Action::Reindex:
      return tr(STR_DELETE_CACHE);
    case Action::RegenerateCover:
      return tr(STR_REGENERATE_COVER);
    case Action::BookInfo:
      return tr(STR_BOOK_INFO);
  }
  return "";
}
}  // namespace

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
  tags_.clear();  // host supplies tags via setInfoTags() after we open
  progressPercent_ = progressPercent;
  return true;
}

bool BookContextMenu::handleInput(ButtonNavigator& nav, const MappedInputManager& input, Action* outAction,
                                  bool* outCancelled) {
  if (!showing_) return false;

  Action actions[OPTIONS_COUNT];
  const int actionCount = collectActions(actions);

  // Allow next/prev wraparound across the option list while modal is up.
  nav.onNext([this, actionCount] { selectedIndex_ = (selectedIndex_ + 1) % actionCount; });
  nav.onPrevious([this, actionCount] { selectedIndex_ = (selectedIndex_ - 1 + actionCount) % actionCount; });

  // Block selecting an option until the initial long-press hold is released.
  if (awaitingRelease_) {
    if (!input.isPressed(MappedInputManager::Button::Confirm)) {
      awaitingRelease_ = false;
    }
    return false;
  }

  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (outAction) *outAction = actions[selectedIndex_];
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
  constexpr int OPTION_H = 34;
  constexpr int LINE_V_PAD = 6;

  const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, title_.c_str(), POPUP_W - H_PAD * 2, 2);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int TITLE_H = LINE_V_PAD + static_cast<int>(titleLines.size()) * lineH + LINE_V_PAD;

  // Build the info lines, skipping any that are empty so the popup doesn't reserve
  // dead space for missing fields (e.g. a book with no author or sitting at the root).
  char infoBuf[96];
  std::string infoLines[4];
  int infoCount = 0;
  if (!author_.empty()) {
    snprintf(infoBuf, sizeof(infoBuf), "%s: %s", tr(STR_BOOK_INFO_AUTHOR), author_.c_str());
    infoLines[infoCount++] = infoBuf;
  }
  if (!tags_.empty()) {
    snprintf(infoBuf, sizeof(infoBuf), "%s: %s", tr(STR_BOOK_INFO_TAGS), tags_.c_str());
    infoLines[infoCount++] = infoBuf;
  }
  const auto lastSlash = path_.rfind('/');
  if (lastSlash != std::string::npos && lastSlash > 0) {
    infoLines[infoCount++] = path_.substr(0, lastSlash);
  }
  if (progressPercent_ < 0) {
    snprintf(infoBuf, sizeof(infoBuf), "%s: %s", tr(STR_BOOK_INFO_PROGRESS), tr(STR_BOOK_INFO_NOT_STARTED));
  } else {
    snprintf(infoBuf, sizeof(infoBuf), "%s: %d%%", tr(STR_BOOK_INFO_PROGRESS), progressPercent_);
  }
  infoLines[infoCount++] = infoBuf;

  Action actions[OPTIONS_COUNT];
  const int actionCount = collectActions(actions);

  const int infoBlockH = infoCount * INFO_H;
  const int POPUP_H = TITLE_H + infoBlockH + actionCount * OPTION_H + H_PAD;
  const int px = (pageWidth - POPUP_W) / 2;
  const int py = (pageHeight - POPUP_H) / 2;

  renderer.fillRect(px, py, POPUP_W, POPUP_H, false);
  renderer.drawRect(px, py, POPUP_W, POPUP_H, BORDER, true);

  for (int i = 0; i < static_cast<int>(titleLines.size()); i++) {
    renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + LINE_V_PAD + i * lineH, titleLines[i].c_str(), true);
  }
  renderer.drawLine(px + BORDER, py + TITLE_H, px + POPUP_W - BORDER - 1, py + TITLE_H);

  for (int i = 0; i < infoCount; i++) {
    const std::string line = renderer.truncatedText(UI_10_FONT_ID, infoLines[i].c_str(), POPUP_W - H_PAD * 2);
    renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + TITLE_H + i * INFO_H + (INFO_H - lineH) / 2, line.c_str());
  }

  renderer.drawLine(px + BORDER, py + TITLE_H + infoBlockH, px + POPUP_W - BORDER - 1, py + TITLE_H + infoBlockH);

  for (int i = 0; i < actionCount; i++) {
    const int optY = py + TITLE_H + infoBlockH + i * OPTION_H;
    if (i == selectedIndex_) {
      renderer.fillRect(px + BORDER, optY, POPUP_W - BORDER * 2, OPTION_H, true);
    }
    renderer.drawText(UI_10_FONT_ID, px + H_PAD * 2, optY + (OPTION_H - lineH) / 2, labelForAction(actions[i]),
                      i != selectedIndex_);
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
