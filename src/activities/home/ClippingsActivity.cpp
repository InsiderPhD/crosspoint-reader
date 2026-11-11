#include "ClippingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/EpubReaderClippingListActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

void ClippingsActivity::onEnter() {
  Activity::onEnter();
  // Entered from a Home press: its Back may still be held / about to release.
  waitForBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  selectedIndex = 0;
  loadBooks();
  requestUpdate();
}

void ClippingsActivity::onExit() {
  books.clear();
  books.shrink_to_fit();
  Activity::onExit();
}

void ClippingsActivity::loadBooks() {
  books.clear();
  books.reserve(16);  // typical library; getAllClippedBooks push_backs one per clipped book
  ClippingStore::getAllClippedBooks(books);
  std::sort(books.begin(), books.end(),
            [](const ClippedBookEntry& a, const ClippedBookEntry& b) { return a.bookTitle < b.bookTitle; });
  if (selectedIndex >= static_cast<int>(books.size())) {
    selectedIndex = std::max(0, static_cast<int>(books.size()) - 1);
  }
}

void ClippingsActivity::openSelectedBook() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(books.size())) return;
  const ClippedBookEntry& book = books[static_cast<size_t>(selectedIndex)];
  // Open on a clipping launches the reader (replacing this screen); the
  // handler only runs on Back.
  startActivityForResult(std::make_unique<EpubReaderClippingListActivity>(renderer, mappedInput, book.bookPath,
                                                                          book.bookTitle, book.bookAuthor),
                         [this](const ActivityResult&) {
                           // Clippings may have been deleted there: refresh the counts.
                           loadBooks();
                           waitForBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
                           requestUpdate();
                         });
}

void ClippingsActivity::loop() {
  if (waitForBackRelease) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      return;
    }
    waitForBackRelease = false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const int count = static_cast<int>(books.size());
  if (count == 0) return;

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), count, selectedIndex, /*hasSubtitle=*/true, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedIndex = tappedIndex;
      openSelectedBook();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelectedBook();
    return;
  }

  const int pageItems = GUI.listGeometry(listRect(), selectedIndex, /*hasSubtitle=*/true).pageItems;
  buttonNavigator.onNextRelease([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });
  if (TouchListNav::pageSwipe(mappedInput, count, pageItems, selectedIndex)) {
    requestUpdate();
    return;
  }
  buttonNavigator.onNextContinuous([this, count, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, count, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, count, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, count, pageItems);
    requestUpdate();
  });
}

// Shared by render() and loop()'s tap hit-testing so the two can never disagree.
Rect ClippingsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void ClippingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLIPPINGS));

  if (books.empty()) {
    const Rect r = listRect();
    renderer.drawCenteredText(UI_10_FONT_ID, r.y + 20, tr(STR_NO_CLIPPINGS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    GUI.drawList(
        renderer, listRect(), static_cast<int>(books.size()), selectedIndex,
        [this](const int index) { return books[index].bookTitle; },
        [this](const int index) { return books[index].bookAuthor; }, nullptr,
        [this](const int index) { return std::to_string(books[index].count); }, false);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
