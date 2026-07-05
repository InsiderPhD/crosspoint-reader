#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "components/BookContextMenu.h"
#include "util/ButtonNavigator.h"

// Whole-library "grouped folders" browse mode, reached via SETTINGS.folderView
// (FOLDER_VIEW_TAGS, FOLDER_VIEW_AUTHORS or FOLDER_VIEW_SERIES). Presents a two-level view:
//   Level 0 — one "folder" per group key (plus an ungrouped catch-all folder).
//   Level 1 — the books in the selected group, opened like a folder's contents.
//
// Three grouping modes share this activity because everything but the key differs:
//   Tags    — key(s) from Epub::getTags(); a book with N tags appears in N folders;
//             the catch-all is "Untagged".
//   Authors — key(s) from Epub/Xtc getAuthor(), split on '&' (co-author separator). A name
//             is "Last, First", so the comma is NOT a split point; the catch-all is
//             "Unknown Author".
//   Series  — a single key from Epub::getSeriesName() (never ", "-joined, and may itself
//             contain a comma, so it is NOT split); the catch-all is "No Series". Within a
//             series, books order by Epub::getSeriesIndex() (numeric) rather than filename.
// splitKeys() picks the delimiter per mode: ',' for tags (ContentOpfParser ", "-joins
// dc:subject), '&' for authors, and none for series.
//
// Enumeration reuses LibraryScan (the same cached whole-SD index LibraryActivity
// uses). Each book's key string is read once at load and kept in `bookKeys`
// (parallel to `bookPaths`) — the same per-book std::string footprint
// LibraryActivity already holds for its author/tag sorts — so entering a folder
// filters in memory with no re-scan.
class GroupBrowserActivity final : public Activity {
 public:
  enum class GroupMode { Tags, Authors, Series };

  GroupBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, GroupMode mode)
      : Activity("GroupBrowser", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const GroupMode mode;

  ButtonNavigator buttonNavigator;
  BookContextMenu contextMenu;

  // Whole-library book paths and their raw key strings (tags = ", "-joined; author =
  // a single name). Parallel vectors; bookKeys[i] is empty when the book has no
  // cached key (unopened book, or genuinely missing metadata).
  std::vector<std::string> bookPaths;
  std::vector<std::string> bookKeys;

  // Series mode only: parsed Epub::getSeriesIndex() per book, parallel to bookPaths.
  // -1 means no/unparseable index (sorts after indexed books). Left empty in Tag/Author
  // mode so it costs nothing there. A float is 4 bytes vs a std::string's ~16-32.
  std::vector<float> bookSeriesIndex;

  // Level-0 folders: unique group display strings (first-seen casing), alpha-sorted,
  // with the ungrouped catch-all ("Untagged"/"Unknown Author") appended last when any
  // book is ungrouped. Counts are parallel to `groups`.
  std::vector<std::string> groups;
  std::vector<uint16_t> groupCounts;
  bool hasUngrouped = false;

  // Level-1: indices into bookPaths for the currently opened group. selectedGroupIndex
  // < 0 means we're at level 0 (the folder list). Titles are the filename stem
  // (folder-browser convention), computed on the fly from bookPaths.
  int selectedGroupIndex = -1;
  std::vector<uint16_t> groupBookIdx;

  size_t selectorIndex = 0;
  // Deferred load: onEnter sets this so the first render can paint a "Loading…"
  // popup before the (potentially slow) whole-library metadata pass.
  bool initialLoadPending = false;
  // True when entered while Confirm was held (typical from the Home menu press).
  // Swallow that first Confirm release so we don't immediately open the first folder.
  bool lockNextConfirmRelease = false;

  // Read every book's cached key (whole-library SD pass) then build the folders.
  void loadLibraryKeys();
  // Build groups/groupCounts/hasUngrouped from the in-memory bookKeys (no SD access).
  void computeGroups();
  // Split a raw ", "-joined key string into trimmed, deduped group keys (tags or
  // co-authors). Empty output = ungrouped.
  void splitKeys(const std::string& raw, std::vector<std::string>& out) const;
  // Rebuild groupBookIdx for `selectedGroupIndex` from in-memory data.
  void buildGroupBookList();
  // Apply a context-menu action to `path`, then refresh the current list.
  void dispatchBookAction(BookContextMenu::Action action, const std::string& path, const std::string& title);

  const char* headerLabel() const;
  const char* ungroupedLabel() const;

  bool atGroupList() const { return selectedGroupIndex < 0; }
  bool isUngroupedIndex(int idx) const { return hasUngrouped && idx == static_cast<int>(groups.size()) - 1; }
};
