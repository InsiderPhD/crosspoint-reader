#pragma once

#include <Epub.h>

#include <memory>
#include <vector>

#include "BookmarkEntry.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderBookmarksActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int currentSpinePageCount = 0;
  int selectorIndex = 0;
  std::vector<BookmarkEntry> bookmarks;
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

 private:
  int getGutterBottom(const GfxRenderer& renderer);
  int getListHeight(const GfxRenderer& renderer);
};
