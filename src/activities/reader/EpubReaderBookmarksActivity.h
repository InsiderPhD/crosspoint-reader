#pragma once

#include <Epub.h>

#include <memory>
#include <vector>

#include "BookmarkEntry.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class EpubReaderBookmarksActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int currentSpinePageCount = 0;
  int selectorIndex = 0;
  std::vector<BookmarkEntry> bookmarks;
  int confirmingDelete = 0;  // 0 = hide dialog, 1 = show dialog, 2 = allow confirmation to delete
  bool deleteHoldTriggered = false;

 public:
  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                       const int currentSpineIndex, const int currentSpinePageCount)
      : Activity("EpubReaderBookmarks", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentSpinePageCount(currentSpinePageCount) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  int getGutterBottom(const GfxRenderer& renderer) const;
  int getListHeight(const GfxRenderer& renderer) const;
  Rect listRect() const;
  void openSelectedBookmark();
  // The hold-Confirm delete, also fired by a Full Touch long tap on a row.
  // Returns true when a bookmark was actually removed.
  bool deleteSelectedBookmark();
};
