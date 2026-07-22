#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "components/BookContextMenu.h"
#include "components/SortMenu.h"
#include "sorting/SortMode.h"
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
  SortMenu sortMenu;

  // Active sort for the books inside a folder. Initialised in onEnter from
  // SETTINGS.groupSortMode, falling back to the per-grouping default (Series order for
  // Series, Name A-Z otherwise) when unset or not offered in this mode. See sortModesForMode().
  SortMode currentSort = SortMode::AlphabeticAsc;

  // Whole-library book paths and their raw key strings (tags = ", "-joined; author =
  // a single name). Parallel vectors; bookKeys[i] is empty when the book has no
  // cached key (unopened book, or genuinely missing metadata).
  std::vector<std::string> bookPaths;
  std::vector<std::string> bookKeys;

  // Series mode only: parsed Epub::getSeriesIndex() per book, parallel to bookPaths.
  // -1 means no/unparseable index (sorts after indexed books). Left empty in Tag/Author
  // mode so it costs nothing there. A float is 4 bytes vs a std::string's ~16-32.
  std::vector<float> bookSeriesIndex;

  // Level-0 folders: unique group display strings (first-seen casing), alpha-sorted.
  // Counts are parallel to `groups`. Ungrouped books are NOT a folder — they sit loose
  // in `rootBookIdx` and render after the folders (see the root list layout below).
  std::vector<std::string> groups;
  std::vector<uint16_t> groupCounts;

  // Ungrouped ("no tag/author/series") books, as indices into bookPaths, sorted by
  // currentSort. Shown loose at the root, immediately after the folders. The root list is
  // therefore [folders 0..groups.size()) then [loose books] — folders always come first.
  std::vector<uint16_t> rootBookIdx;

  // Level-1: indices into bookPaths for the currently opened group. selectedGroupIndex
  // < 0 means we're at the root list (folders + loose books). Titles are the filename stem
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
  // Build groups/groupCounts and the loose rootBookIdx from the in-memory bookKeys (no SD).
  void computeGroups();
  // Split a raw ", "-joined key string into trimmed, deduped group keys (tags or
  // co-authors). Empty output = ungrouped.
  void splitKeys(const std::string& raw, std::vector<std::string>& out) const;
  // Rebuild groupBookIdx for `selectedGroupIndex` from in-memory data.
  void buildGroupBookList();
  // Sort a set of bookPaths indices in place by currentSort (shared by the folder list and
  // the loose root books). Date-added is stat'd on demand only when that mode is active.
  void sortBookIndices(std::vector<uint16_t>& idx) const;
  // Apply a context-menu action to `path`, then refresh the current list.
  void dispatchBookAction(BookContextMenu::Action action, const std::string& path, const std::string& title);

  const char* headerLabel() const;

  // Root list layout: folders occupy [0, groups.size()), loose books follow.
  size_t rootListSize() const { return groups.size() + rootBookIdx.size(); }
  bool isFolderRow(size_t row) const { return row < groups.size(); }
  // Path of the book the selector is on, or "" when it's on a folder / out of range.
  std::string selectedBookPath() const;

  // The sort fields offered for the current grouping mode (Series adds "Series order";
  // Tags/Authors offer the name/opened/progress/date-added fields). Returns a pointer to a
  // static constexpr array and writes its length to `count`.
  const SortOption* sortOptionsForMode(int& count) const;
  // The default sort when SETTINGS.groupSortMode is unset/invalid for this grouping mode.
  SortMode defaultSortForMode() const;

  // True at the root list (folders + loose books); false inside an opened folder.
  bool atGroupList() const { return selectedGroupIndex < 0; }
};
