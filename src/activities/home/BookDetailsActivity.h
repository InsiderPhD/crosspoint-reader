#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

  void loadMetadata();
  void freeDescBuffer();

 public:
  explicit BookDetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                               std::string title, std::string author, int progressPercent)
      : Activity("BookDetails", renderer, mappedInput),
        bookPath(std::move(bookPath)),
        title(std::move(title)),
        author(std::move(author)),
        progressPercent(progressPercent) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
