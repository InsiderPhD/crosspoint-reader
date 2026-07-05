#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

// Full-page book details: Lyra-sized cover on the left, a label/value metadata table
// (series, tags, author, language, progress) on the right, and the book description
// below, scrollable. Modelled on ReadingStatsDetailActivity.
//
// Memory: the description is read once into a single bounded malloc buffer
// (Epub::MAX_DESCRIPTION_BYTES) and freed in onExit(). It is never held as a
// std::string nor cached inline in book.bin, so the Library hot path is unaffected.
class BookDetailsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  std::string bookPath;
  std::string title;
  std::string author;
  std::string seriesName;
  std::string seriesNumber;
  std::string bookshelf;
  std::string categories;  // BookFusion only (sidecar)
  std::string lists;       // BookFusion only (sidecar)
  std::string published;
  std::string publisher;
  std::string language;
  std::string tags;
  std::string rating;
  int progressPercent;

  std::string coverPath;
  char* descBuffer = nullptr;  // null-terminated; freed in onExit()
  size_t descLength = 0;

  int scrollOffset = 0;      // first visible description line
  int descVisibleLines = 1;  // updated each render; used for page-step scrolling
  bool loading = false;      // true while loadMetadata() runs (may build cache / cover)
  bool waitForConfirmRelease = false;
  bool waitForBackRelease = false;

  // Optional sibling navigation: Left/Right step to the prev/next book in the
  // list the user was browsing. Paths are NOT copied for Library/GroupBrowser —
  // siblingPaths points at the parent activity's long-lived vector (the parent
  // stays alive on the activity stack while this child runs). siblingOrder maps
  // a sequence position to an index into *siblingPaths (empty = identity order).
  // FileBrowser has no full-path vector, so it hands over a folder-scoped copy
  // in ownedSiblingPaths and siblingPaths points at that member.
  const std::vector<std::string>* siblingPaths = nullptr;
  std::vector<std::string> ownedSiblingPaths;
  std::vector<uint16_t> siblingOrder;
  int siblingPos = -1;

  void loadMetadata();
  void freeDescBuffer();

  [[nodiscard]] int siblingCount() const;
  [[nodiscard]] const std::string* siblingPathAt(int pos) const;
  void navigateToSibling(int newPos);
  void resetForBook(const std::string& newPath);
  void applyBaseInfoFromPath();

 public:
  explicit BookDetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                               std::string title, std::string author, int progressPercent)
      : Activity("BookDetails", renderer, mappedInput),
        bookPath(std::move(bookPath)),
        title(std::move(title)),
        author(std::move(author)),
        progressPercent(progressPercent) {}

  // Library / GroupBrowser: reference the parent's long-lived path vector (no copy).
  // `order` maps display position -> index into *paths; `pos` is the current position.
  void setSiblingsRef(const std::vector<std::string>* paths, std::vector<uint16_t> order, int pos) {
    siblingPaths = paths;
    siblingOrder = std::move(order);
    siblingPos = pos;
  }

  // FileBrowser: take ownership of a folder-scoped full-path list (identity order).
  void setSiblingsOwned(std::vector<std::string> paths, int pos) {
    ownedSiblingPaths = std::move(paths);
    siblingPaths = &ownedSiblingPaths;
    siblingOrder.clear();
    siblingPos = pos;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
