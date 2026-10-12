#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Memory.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/ListPickerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/TouchListNav.h"

namespace {
constexpr int ENTER_ACTIONS_MODE_MS = 700;
// Rows of the hold-Confirm action list.
constexpr int ACTION_RENAME = 0;
constexpr int ACTION_DELETE = 1;
constexpr int ACTION_COUNT = 2;
constexpr int DELETE_MODE_OFF = 0;
constexpr int DELETE_MODE_DISPLAY = 1;
constexpr int DELETE_MODE_CONFIRM = 2;
constexpr int LINE_HEIGHT = 60;

// Rows the theme will actually draw in `rect` — page counter strip already
// taken out, so a page step moves by exactly what is on screen.
int getPageItemsForBookmarkList(const Rect& rect) {
  return std::max(1, GUI.listGeometry(rect, 0, /*hasSubtitle=*/true).pageItems);
}
}  // namespace

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epubPath);
  if (Storage.exists(path.c_str())) {
    String json = Storage.readFile(path.c_str());
    if (json.isEmpty()) {
      LOG_ERR("EPB", "Failed to load bookmarks from %s. Empty bookmark file", path.c_str());
      bookmarks.clear();
      bookmarks.shrink_to_fit();
    } else {
      JsonSettingsIO::loadBookmarks(bookmarks, json.c_str());
      for (auto& bookmark : bookmarks) {
        const CrossPointPosition pos = ProgressMapper::toCrossPoint(epub, {bookmark.xpath, bookmark.percentage},
                                                                    currentSpineIndex, currentSpinePageCount);
        bookmark.computedSpineIndex = static_cast<uint16_t>(std::max(0, pos.spineIndex));
        bookmark.computedChapterPageCount = static_cast<uint16_t>(std::max(0, pos.totalPages));
        bookmark.computedChapterProgress = static_cast<uint16_t>(std::max(0, pos.pageNumber));
      }
    }
  } else {
    bookmarks.clear();
    bookmarks.shrink_to_fit();
  }

  requestUpdate();
}

void EpubReaderBookmarksActivity::onExit() { Activity::onExit(); }

int EpubReaderBookmarksActivity::getGutterBottom(const GfxRenderer& renderer) const {
  const auto orientation = renderer.getOrientation();
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  return isPortrait ? 75 : 40;
}

int EpubReaderBookmarksActivity::getListHeight(const GfxRenderer& renderer) const {
  return renderer.getScreenHeight() - getGutterBottom(renderer) - LINE_HEIGHT;
}

// List body between the title row and the bottom help text. Shared by render()
// and the loop()'s tap hit-testing so the two can never disagree.
Rect EpubReaderBookmarksActivity::listRect() const {
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;

  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isPortraitInverted ? 50 : 0;
  return Rect{contentX, contentY + LINE_HEIGHT, contentWidth, getListHeight(renderer)};
}

bool EpubReaderBookmarksActivity::deleteSelectedBookmark() {
  if (bookmarks.empty()) {
    return false;
  }
  if (activityManager.isOnRenderTask()) {
    LOG_ERR("EPB", "Bookmark delete requested on render task; ignoring");
    return false;
  }

  {
    RenderLock lock(*this);
    bookmarks.erase(bookmarks.begin() + selectorIndex);
    const std::string path = BookmarkUtil::getBookmarkPath(epubPath);
    Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
    if (!JsonSettingsIO::saveBookmarks(bookmarks, path.c_str())) {
      LOG_ERR("EPB", "Failed to save bookmarks after delete");
    }

    if (selectorIndex >= static_cast<int>(bookmarks.size()) && selectorIndex > 0) {
      selectorIndex--;
    }
  }

  requestUpdate();
  return true;
}

void EpubReaderBookmarksActivity::armStaleReleaseGuard() {
  swallowStaleRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                        mappedInput.isPressed(MappedInputManager::Button::Back);
}

void EpubReaderBookmarksActivity::showBookmarkActions() {
  if (bookmarks.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(bookmarks.size())) {
    return;
  }
  // Headed by the bookmark's own name when it has one, so the list says which
  // bookmark is about to be renamed or deleted. The picker keeps only the
  // pointer: the name is not touched until the picker has closed (rename and
  // delete both run from its result handler), so nothing is copied.
  const std::string& name = bookmarks[selectorIndex].name;
  const char* title = name.empty() ? tr(STR_BOOKMARK) : name.c_str();
  // Heap: one small activity object, freed when the list closes.
  auto picker = makeUniqueNoThrow<ListPickerActivity>(
      renderer, mappedInput, title, ACTION_COUNT, 0,
      [](int index) { return std::string(index == ACTION_RENAME ? tr(STR_RENAME) : tr(STR_DELETE)); }, nullptr,
      /*markInitial=*/false);
  if (!picker) {
    LOG_ERR("EPB", "OOM: bookmark action list");
    return;
  }
  startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
    armStaleReleaseGuard();
    if (result.isCancelled) {
      return;
    }
    const int action = std::get<ListPickResult>(result.data).index;
    if (action == ACTION_RENAME) {
      startRename();
    } else if (action == ACTION_DELETE) {
      deleteSelectedBookmark();
    }
  });
}

void EpubReaderBookmarksActivity::startRename() {
  if (bookmarks.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(bookmarks.size())) {
    return;
  }
  const int renameIndex = selectorIndex;
  // Heap: the keyboard activity and its text, both gone before the list is
  // back on screen. Nothing outlives the rename.
  auto keyboard =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME), bookmarks[renameIndex].name,
                                               BookmarkEntry::MAX_NAME_LENGTH, InputType::Text);
  if (!keyboard) {
    LOG_ERR("EPB", "OOM: bookmark rename keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this, renameIndex](const ActivityResult& result) {
    armStaleReleaseGuard();
    if (result.isCancelled || renameIndex >= static_cast<int>(bookmarks.size())) {
      return;
    }
    std::string name = std::get<KeyboardResult>(result.data).text;
    if (name.size() > BookmarkEntry::MAX_NAME_LENGTH) {
      name.resize(BookmarkEntry::MAX_NAME_LENGTH);
    }
    if (name == bookmarks[renameIndex].name) {
      return;
    }

    RenderLock lock(*this);
    // An empty name is allowed: it puts the page summary back as the label.
    std::swap(bookmarks[renameIndex].name, name);
    const std::string path = BookmarkUtil::getBookmarkPath(epubPath);
    Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
    if (!JsonSettingsIO::saveBookmarks(bookmarks, path.c_str())) {
      LOG_ERR("EPB", "Failed to save bookmarks after rename");
      std::swap(bookmarks[renameIndex].name, name);
    }
  });
}

void EpubReaderBookmarksActivity::loop() {
  if (swallowStaleRelease) {
    if (mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.isPressed(MappedInputManager::Button::Back)) {
      return;
    }
    swallowStaleRelease = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      return;
    }
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() > ENTER_ACTIONS_MODE_MS &&
      !actionsHoldTriggered && !bookmarks.empty()) {
    actionsHoldTriggered = true;
    showBookmarkActions();
    return;
  }

  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    actionsHoldTriggered = false;
  }

#if FREEINK_DEVICE_X4PRO
  // Full Touch: a long tap on a bookmark row opens that row's action list,
  // like the hold-Confirm above. The contact is suppressed so the lift does not
  // also count as a tap.
  if (CrossPointSettings::FULL_TOUCH_UI && !bookmarks.empty()) {
    int lx, ly;
    if (mappedInput.wasTouchLongPressPoint(lx, ly)) {
      const int index =
          GUI.hitTestList(listRect(), static_cast<int>(bookmarks.size()), selectorIndex, /*hasSubtitle=*/true, lx, ly);
      if (index >= 0) {
        selectorIndex = index;
        mappedInput.suppressTouchContact();
        showBookmarkActions();
        return;
      }
    }
  }
#endif

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), static_cast<int>(bookmarks.size()), selectorIndex,
                               /*hasSubtitle=*/true, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectorIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectorIndex = tappedIndex;
      openSelectedBookmark();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelectedBookmark();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, static_cast<int>(bookmarks.size()));
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, static_cast<int>(bookmarks.size()));
    requestUpdate();
  });

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  if (TouchListNav::pageSwipe(mappedInput, static_cast<int>(bookmarks.size()), getPageItemsForBookmarkList(listRect()),
                              selectorIndex)) {
    requestUpdate();
    return;
  }

  buttonNavigator.onNextContinuous([this] {
    const int pageItems = getPageItemsForBookmarkList(listRect());
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, static_cast<int>(bookmarks.size()), pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    const int pageItems = getPageItemsForBookmarkList(listRect());
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, static_cast<int>(bookmarks.size()), pageItems);
    requestUpdate();
  });
}

// The Confirm short-press body, also fired by a Full Touch tap on the selected row.
void EpubReaderBookmarksActivity::openSelectedBookmark() {
  if (bookmarks.empty()) {
    return;
  }

  const auto bookmark = bookmarks.at(selectorIndex);
  const CrossPointPosition pos = ProgressMapper::toCrossPoint(epub, {bookmark.xpath, bookmark.percentage},
                                                              currentSpineIndex, currentSpinePageCount);
  setResult(SyncResult{pos.spineIndex, pos.pageNumber, -1.0f});
  finish();
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();

  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;

  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int contentY = isPortraitInverted ? 50 : 0;
  const int hintGutterBottom = getGutterBottom(renderer);

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  const auto getBookmarkTitle = [this](int index) {
    const auto& bookmark = bookmarks.at(index);
    return bookmark.name.empty() ? bookmark.summary : bookmark.name;
  };

  const auto getBookmarkSubtitle = [this](int index) {
    auto bookmark = bookmarks.at(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index);
    auto tocIndex = epub->getTocIndexForSpineIndex(bookmark.computedSpineIndex);
    auto tocTitle = (tocIndex >= 0) ? (epub->getTocItem(tocIndex)).title : tr(STR_UNNAMED);
    return std::to_string((int)(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f)) + "% - " +
           std::to_string(bookmark.computedChapterProgress + 1) + "/" +
           std::to_string(bookmark.computedChapterPageCount) + " - " + tocTitle;
  };

  const auto getBookmarkIcon = [](int) { return UIIcon::None; };

  if (!bookmarks.empty()) {
    GUI.drawList(renderer, listRect(), static_cast<int>(bookmarks.size()), selectorIndex, getBookmarkTitle,
                 getBookmarkSubtitle, getBookmarkIcon);

    GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                     tr(STR_HOLD_CONFIRM_FOR_ACTIONS));
  } else {
    GUI.drawHelpText(renderer, Rect{contentX, LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                     tr(STR_BOOKMARK_INSTRUCTIONS));
  }

  const char* backLabel = tr(STR_BACK);
  const char* confirmLabel = bookmarks.empty() ? "" : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) {
    renderer.invertScreen();
  }
  renderer.displayBuffer();
}
