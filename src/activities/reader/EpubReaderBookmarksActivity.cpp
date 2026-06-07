#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
constexpr int LINE_HEIGHT = 60;

int getPageItemsForBookmarkList(const GfxRenderer& renderer, const int listHeight) {
  const int rowHeight = std::max(1, UITheme::getInstance().getMetrics().listWithSubtitleRowHeight);
  return std::max(1, listHeight / rowHeight);
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

int EpubReaderBookmarksActivity::getGutterBottom(const GfxRenderer& renderer) {
  const auto orientation = renderer.getOrientation();
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  return isPortrait ? 75 : 40;
}

int EpubReaderBookmarksActivity::getListHeight(const GfxRenderer& renderer) {
  return renderer.getScreenHeight() - getGutterBottom(renderer) - LINE_HEIGHT;
}

void EpubReaderBookmarksActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS &&
      !deleteHoldTriggered && !bookmarks.empty()) {
    if (activityManager.isOnRenderTask()) {
      LOG_ERR("EPB", "Bookmark delete requested on render task; ignoring");
      return;
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

    deleteHoldTriggered = true;
    requestUpdate();
    return;
  }

  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    deleteHoldTriggered = false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (bookmarks.empty()) {
      return;
    }

    const auto bookmark = bookmarks.at(selectorIndex);
    const CrossPointPosition pos = ProgressMapper::toCrossPoint(epub, {bookmark.xpath, bookmark.percentage},
                                                                 currentSpineIndex, currentSpinePageCount);
    setResult(SyncResult{pos.spineIndex, pos.pageNumber, -1.0f});
    finish();
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

  buttonNavigator.onNextContinuous([this] {
    const int pageItems = getPageItemsForBookmarkList(renderer, getListHeight(renderer));
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, static_cast<int>(bookmarks.size()),
                                                    pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    const int pageItems = getPageItemsForBookmarkList(renderer, getListHeight(renderer));
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, static_cast<int>(bookmarks.size()),
                                                        pageItems);
    requestUpdate();
  });
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
  const int listY = contentY + LINE_HEIGHT;
  const int listHeight = getListHeight(renderer);

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  const auto getBookmarkTitle = [this](int index) { return bookmarks.at(index).summary; };

  const auto getBookmarkSubtitle = [this](int index) {
    const auto bookmark = bookmarks.at(index);
    const int tocIndex = epub->getTocIndexForSpineIndex(bookmark.computedSpineIndex);
    const auto tocTitle = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : std::string(tr(STR_UNNAMED));
    return std::to_string(static_cast<int>(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f)) + "% - " +
           std::to_string(bookmark.computedChapterProgress + 1) + "/" +
           std::to_string(bookmark.computedChapterPageCount) + " - " + tocTitle;
  };

  const auto getBookmarkIcon = [](int) { return UIIcon::None; };

  if (!bookmarks.empty()) {
    GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, static_cast<int>(bookmarks.size()),
                 selectorIndex, getBookmarkTitle, getBookmarkSubtitle, getBookmarkIcon);

    GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                     tr(STR_HOLD_CONFIRM_TO_DELETE));
  } else {
    GUI.drawHelpText(renderer, Rect{contentX, LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT}, tr(STR_BOOKMARK_INSTRUCTIONS));
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
