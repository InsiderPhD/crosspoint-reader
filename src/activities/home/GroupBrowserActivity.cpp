#include "GroupBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

#include "../util/ConfirmationActivity.h"
#include "BookDetailsActivity.h"
#include "CrossPointSettings.h"
#include "LibraryScan.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/WifiTimeSync.h"

namespace {
std::string stemOf(const std::string& path) {
  const auto slash = path.find_last_of('/');
  std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const auto dot = name.rfind('.');
  return (dot == std::string::npos) ? name : name.substr(0, dot);
}

bool iEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

bool iLess(const std::string& a, const std::string& b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const int ca = std::tolower(static_cast<unsigned char>(a[i]));
    const int cb = std::tolower(static_cast<unsigned char>(b[i]));
    if (ca != cb) return ca < cb;
  }
  return a.size() < b.size();
}

std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// Progress percent for a path from RecentBooksStore, or -1 if not tracked.
int progressFor(const std::string& path) {
  const auto& recents = RECENT_BOOKS.getBooks();
  auto it = std::find_if(recents.begin(), recents.end(), [&path](const RecentBook& b) { return b.path == path; });
  return (it != recents.end()) ? it->progressPercent : -1;
}
}  // namespace

const char* GroupBrowserActivity::headerLabel() const {
  switch (mode) {
    case GroupMode::Tags:
      return tr(STR_TAGS);
    case GroupMode::Authors:
      return tr(STR_AUTHORS);
    case GroupMode::Series:
      return tr(STR_SERIES);
  }
  return tr(STR_TAGS);
}

const char* GroupBrowserActivity::ungroupedLabel() const {
  switch (mode) {
    case GroupMode::Tags:
      return tr(STR_UNTAGGED);
    case GroupMode::Authors:
      return tr(STR_UNKNOWN_AUTHOR);
    case GroupMode::Series:
      return tr(STR_NO_SERIES);
  }
  return tr(STR_UNTAGGED);
}

void GroupBrowserActivity::splitKeys(const std::string& raw, std::vector<std::string>& out) const {
  out.clear();
  if (mode == GroupMode::Series) {
    // A series name is a single value that is never ", "-joined and may itself contain a
    // comma (e.g. "Wolf, Star"), so — unlike tags/authors — we take the whole trimmed
    // string as one key. Empty output = ungrouped ("No Series").
    std::string s = trimmed(raw);
    if (!s.empty()) out.push_back(std::move(s));
    return;
  }
  // Delimiter differs by mode:
  //   Tags    — multiple dc:subject are ", "-joined by ContentOpfParser, so split on ','.
  //   Authors — a name is written "Last, First" (comma = intra-name), while co-authors are
  //             separated by '&', so split on '&' — NOT ',' — or "Bryson, Bill" would wrongly
  //             become two folders "Bryson" and "Bill".
  // Trim and dedupe case-insensitively within the book.
  // NB (Authors): when an EPUB uses multiple <dc:creator> elements, ContentOpfParser ", "-joins
  // them, so those co-authors land in one shared folder rather than one each — a comma there is
  // indistinguishable from an intra-name comma. Books that separate co-authors with '&' split
  // correctly.
  const char delim = (mode == GroupMode::Authors) ? '&' : ',';
  size_t i = 0;
  const size_t n = raw.size();
  while (i <= n) {
    const size_t sep = raw.find(delim, i);
    const size_t end = (sep == std::string::npos) ? n : sep;
    std::string key = trimmed(raw.substr(i, end - i));
    if (!key.empty()) {
      bool dup = false;
      for (const auto& existing : out) {
        if (iEquals(existing, key)) {
          dup = true;
          break;
        }
      }
      if (!dup) out.push_back(std::move(key));
    }
    if (sep == std::string::npos) break;
    i = sep + 1;
  }
}

void GroupBrowserActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  selectedGroupIndex = -1;
  // If Confirm was held while this activity opened (typical when launched from the
  // Home menu), ignore its release — otherwise we'd immediately open the first folder.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  // Defer the whole-library metadata pass to the first render so we can paint a
  // "Loading…" popup before it starts (mirrors LibraryActivity::initialLoadPending).
  initialLoadPending = true;
  requestUpdate();
}

void GroupBrowserActivity::onExit() {
  Activity::onExit();
  bookPaths.clear();
  bookPaths.shrink_to_fit();
  bookKeys.clear();
  bookKeys.shrink_to_fit();
  bookSeriesIndex.clear();
  bookSeriesIndex.shrink_to_fit();
  groups.clear();
  groupCounts.clear();
  groupBookIdx.clear();
}

void GroupBrowserActivity::loadLibraryKeys() {
  // Preempt the background boot-time NTP sync (if still running) so its WiFi
  // stack heap is freed before this recache's heavy per-book allocations —
  // avoids a cold-boot OOM/contention crash. No-op after the ~10s boot window.
  WifiTimeSync::preempt();
  LibraryScan::enumerateBooks(bookPaths);
  bookKeys.assign(bookPaths.size(), std::string{});
  // Only series mode needs a per-book numeric sort key; leave it empty otherwise.
  if (mode == GroupMode::Series) bookSeriesIndex.assign(bookPaths.size(), -1.0f);

  const int total = static_cast<int>(bookPaths.size());
  // TEMP diagnostic (crash triage): free heap vs largest contiguous block during the
  // full metadata recache. A high free heap with a small largest block => fragmentation
  // (alloc-abort under -fno-exceptions); a healthy largest block right up to a crash =>
  // not memory. Remove once the browse crash is root-caused.
  LOG_DBG("MEMDIAG", "recache start: %d books, free=%u largest=%u", total, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  for (size_t i = 0; i < bookPaths.size(); ++i) {
    if (i % 20 == 0) {
      LOG_DBG("MEMDIAG", "recache i=%zu free=%u largest=%u", i, static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    }
    const std::string& path = bookPaths[i];
    bool built = false;
    if (FsHelpers::hasEpubExtension(path)) {
      Epub epub(path, "/.crosspoint");
      // Cache-on-browse: build book.bin if it's missing so never-opened books still
      // group correctly (buildIfMissing=true). Already-cached books just read the
      // existing book.bin (fast). skipLoadingCss=true keeps it to metadata only.
      built = !Storage.exists((epub.getCachePath() + "/book.bin").c_str());
      if (epub.load(true, true)) {
        switch (mode) {
          case GroupMode::Tags:
            bookKeys[i] = epub.getTags();
            break;
          case GroupMode::Authors:
            bookKeys[i] = epub.getAuthor();
            break;
          case GroupMode::Series: {
            bookKeys[i] = epub.getSeriesName();
            const std::string idx = epub.getSeriesIndex();
            if (!idx.empty()) bookSeriesIndex[i] = strtof(idx.c_str(), nullptr);
            break;
          }
        }
      }
    } else if (mode == GroupMode::Authors && FsHelpers::hasXtcExtension(path)) {
      // XTC carries an author but no tags — only relevant in author mode.
      Xtc xtc(path, "/.crosspoint");
      if (xtc.load()) bookKeys[i] = xtc.getAuthor();
    }
    // We're called from render() (holding the RenderLock), so we can paint directly.
    // Only repaint when a book was actually built — a slow step worth showing; when
    // everything is already cached the load stays fast with no refresh spam.
    if (built) {
      char msg[32];
      snprintf(msg, sizeof(msg), "%s %d / %d", tr(STR_CACHING), static_cast<int>(i) + 1, total);
      renderer.clearScreen();
      GUI.drawPopup(renderer, msg);
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      if (SETTINGS.darkMode) renderer.invertScreen();
    }
    // Yield every iteration: a cache-miss triggers a full OPF/TOC parse (~0.5-2s), so
    // yielding only every N books could block the task past the watchdog timeout.
    vTaskDelay(1);
  }

  computeGroups();
}

void GroupBrowserActivity::computeGroups() {
  groups.clear();
  groupCounts.clear();
  hasUngrouped = false;
  uint16_t ungroupedCount = 0;

  // Accumulate (display key, count) pairs, deduped case-insensitively across the
  // whole library. A book contributes once per distinct key it carries.
  std::vector<std::pair<std::string, uint16_t>> acc;
  std::vector<std::string> parts;
  for (const auto& raw : bookKeys) {
    splitKeys(raw, parts);
    if (parts.empty()) {
      hasUngrouped = true;
      if (ungroupedCount < 0xFFFF) ++ungroupedCount;
      continue;
    }
    for (const auto& p : parts) {
      auto it = std::find_if(acc.begin(), acc.end(), [&p](const auto& e) { return iEquals(e.first, p); });
      if (it != acc.end()) {
        if (it->second < 0xFFFF) ++it->second;
      } else {
        acc.emplace_back(p, 1);
      }
    }
  }

  std::sort(acc.begin(), acc.end(), [](const auto& a, const auto& b) { return iLess(a.first, b.first); });

  groups.reserve(acc.size() + 1);
  groupCounts.reserve(acc.size() + 1);
  for (auto& e : acc) {
    groups.push_back(std::move(e.first));
    groupCounts.push_back(e.second);
  }
  if (hasUngrouped) {
    groups.push_back(ungroupedLabel());
    groupCounts.push_back(ungroupedCount);
  }
}

void GroupBrowserActivity::buildGroupBookList() {
  groupBookIdx.clear();
  if (selectedGroupIndex < 0 || selectedGroupIndex >= static_cast<int>(groups.size())) return;

  const bool ungrouped = isUngroupedIndex(selectedGroupIndex);
  const std::string want = groups[selectedGroupIndex];  // copy: groups may reallocate elsewhere

  std::vector<std::string> parts;
  for (size_t i = 0; i < bookPaths.size(); ++i) {
    splitKeys(bookKeys[i], parts);
    bool match = false;
    if (ungrouped) {
      match = parts.empty();
    } else {
      for (const auto& p : parts) {
        if (iEquals(p, want)) {
          match = true;
          break;
        }
      }
    }
    if (match) groupBookIdx.push_back(static_cast<uint16_t>(i));
  }

  if (mode == GroupMode::Series) {
    // Order a series by its numeric index (book 1, 2, 3…). Books with no parseable index
    // (-1) sink below the indexed ones; ties and the un-indexed fall back to filename.
    std::sort(groupBookIdx.begin(), groupBookIdx.end(), [this](uint16_t a, uint16_t b) {
      const float ia = bookSeriesIndex[a];
      const float ib = bookSeriesIndex[b];
      const bool ha = ia >= 0.0f;
      const bool hb = ib >= 0.0f;
      if (ha != hb) return ha;             // indexed books before un-indexed
      if (ha && ia != ib) return ia < ib;  // ascending by series index
      return iLess(stemOf(bookPaths[a]), stemOf(bookPaths[b]));
    });
  } else {
    std::sort(groupBookIdx.begin(), groupBookIdx.end(),
              [this](uint16_t a, uint16_t b) { return iLess(stemOf(bookPaths[a]), stemOf(bookPaths[b])); });
  }
}

void GroupBrowserActivity::dispatchBookAction(BookContextMenu::Action action, const std::string& path,
                                              const std::string& title) {
  switch (action) {
    case BookContextMenu::Action::MarkRead:
      RECENT_BOOKS.updateProgress(path, 100);
      RECENT_BOOKS.saveToFile();
      requestUpdate(true);
      break;
    case BookContextMenu::Action::ResetProgress:
      RECENT_BOOKS.updateProgress(path, -1);
      RECENT_BOOKS.saveToFile();
      requestUpdate(true);
      break;
    case BookContextMenu::Action::Shelve:
      RECENT_BOOKS.removeBook(path);
      RECENT_BOOKS.saveToFile();
      requestUpdate(true);
      break;
    case BookContextMenu::Action::Reindex:
      if (FsHelpers::hasEpubExtension(path)) {
        Storage.removeDir((Epub(path, "/.crosspoint").getCachePath() + "/sections").c_str());
      } else if (FsHelpers::hasXtcExtension(path)) {
        Xtc(path, "/.crosspoint").clearCache();
      }
      requestUpdate(true);
      break;
    case BookContextMenu::Action::RegenerateCover:
      // Covers aren't shown in the list; nothing to redraw here.
      requestUpdate(true);
      break;
    case BookContextMenu::Action::Delete:
      startActivityForResult(std::make_unique<ConfirmationActivity>(
                                 renderer, mappedInput, tr(STR_DELETE_FROM_DEVICE) + std::string("?"), title),
                             [this, path](const ActivityResult& res) mutable {
                               if (res.isCancelled) return;
                               if (FsHelpers::hasEpubExtension(path)) Epub(path, "/.crosspoint").clearCache();
                               Storage.remove(path.c_str());
                               RECENT_BOOKS.removeBook(path);
                               RECENT_BOOKS.saveToFile();
                               LibraryScan::invalidateIndex();  // deleted file must drop from the shared index

                               // Drop from the in-memory library, remembering the current group so we can
                               // stay in it if it still has books after the deletion.
                               const std::string currentGroup =
                                   (selectedGroupIndex >= 0 && selectedGroupIndex < static_cast<int>(groups.size()))
                                       ? groups[selectedGroupIndex]
                                       : std::string{};
                               const bool wasUngrouped =
                                   (selectedGroupIndex >= 0) && isUngroupedIndex(selectedGroupIndex);
                               for (size_t i = 0; i < bookPaths.size(); ++i) {
                                 if (bookPaths[i] == path) {
                                   bookPaths.erase(bookPaths.begin() + i);
                                   bookKeys.erase(bookKeys.begin() + i);
                                   if (i < bookSeriesIndex.size()) bookSeriesIndex.erase(bookSeriesIndex.begin() + i);
                                   break;
                                 }
                               }
                               computeGroups();

                               // Re-find the group we were viewing; fall back to the folder list if it's gone.
                               selectedGroupIndex = -1;
                               for (size_t i = 0; i < groups.size(); ++i) {
                                 const bool thisUngrouped = isUngroupedIndex(static_cast<int>(i));
                                 if ((wasUngrouped && thisUngrouped) ||
                                     (!wasUngrouped && !thisUngrouped && iEquals(groups[i], currentGroup))) {
                                   selectedGroupIndex = static_cast<int>(i);
                                   break;
                                 }
                               }
                               if (selectedGroupIndex >= 0) {
                                 buildGroupBookList();
                                 if (selectorIndex >= groupBookIdx.size())
                                   selectorIndex = groupBookIdx.empty() ? 0 : groupBookIdx.size() - 1;
                               } else {
                                 selectorIndex = 0;
                               }
                               requestUpdate(true);
                             });
      break;
    case BookContextMenu::Action::BookInfo: {
      // Let BookDetails page Left/Right through the current group in display order.
      // bookPaths outlives the child (parent stays on the activity stack); only the
      // uint16 order (a copy of groupBookIdx) is owned by the child.
      std::vector<uint16_t> order = groupBookIdx;
      int pos = static_cast<int>(selectorIndex);
      if (pos < 0 || pos >= static_cast<int>(order.size()) || order[pos] >= bookPaths.size() ||
          bookPaths[order[pos]] != path) {
        pos = 0;
        for (size_t i = 0; i < order.size(); ++i) {
          if (order[i] < bookPaths.size() && bookPaths[order[i]] == path) {
            pos = static_cast<int>(i);
            break;
          }
        }
      }
      auto details = std::make_unique<BookDetailsActivity>(renderer, mappedInput, path, title, contextMenu.author(),
                                                           contextMenu.progressPercent());
      details->setSiblingsRef(&bookPaths, std::move(order), pos);
      startActivityForResult(std::move(details), [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
  }
}

void GroupBrowserActivity::loop() {
  if (initialLoadPending) return;  // First render performs the load.

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  // --- Level 0: group folders ---
  if (atGroupList()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (lockNextConfirmRelease) {
        lockNextConfirmRelease = false;
        return;
      }
      if (!groups.empty() && selectorIndex < groups.size()) {
        selectedGroupIndex = static_cast<int>(selectorIndex);
        buildGroupBookList();
        selectorIndex = 0;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }
    const int listSize = static_cast<int>(groups.size());
    buttonNavigator.onNextPress([this, listSize] {
      selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
      requestUpdate();
    });
    buttonNavigator.onPreviousPress([this, listSize] {
      selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this, listSize, pageItems] {
      selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
      selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
      requestUpdate();
    });
    return;
  }

  // --- Level 1: books in the selected group ---
  if (contextMenu.isOpen()) {
    BookContextMenu::Action action;
    bool cancelled = false;
    if (contextMenu.handleInput(buttonNavigator, mappedInput, &action, &cancelled)) {
      if (!cancelled) {
        dispatchBookAction(action, contextMenu.path(), contextMenu.title());
      } else {
        requestUpdate();
      }
    } else {
      requestUpdate();
    }
    return;
  }

  if (!groupBookIdx.empty() && selectorIndex < groupBookIdx.size()) {
    const std::string path = bookPaths[groupBookIdx[selectorIndex]];
    const std::string title = stemOf(path);
    std::string author;
    const auto& recents = RECENT_BOOKS.getBooks();
    auto it = std::find_if(recents.begin(), recents.end(), [&path](const RecentBook& b) { return b.path == path; });
    if (it != recents.end()) author = it->author;
    if (contextMenu.checkLongPress(mappedInput, path, title, author, progressFor(path))) {
      if (FsHelpers::hasEpubExtension(path)) {
        Epub epub(path, "/.crosspoint");
        if (epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) contextMenu.setInfoTags(epub.getTags());
      }
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (contextMenu.consumeLongPressFlag()) return;
    if (!groupBookIdx.empty() && selectorIndex < groupBookIdx.size()) {
      onSelectBook(bookPaths[groupBookIdx[selectorIndex]]);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Step back up to the folder list, landing on the folder we came from.
    selectorIndex = (selectedGroupIndex >= 0) ? static_cast<size_t>(selectedGroupIndex) : 0;
    selectedGroupIndex = -1;
    groupBookIdx.clear();
    requestUpdate();
    return;
  }

  const int listSize = static_cast<int>(groupBookIdx.size());
  buttonNavigator.onNextPress([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onPreviousPress([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void GroupBrowserActivity::render(RenderLock&&) {
  // Deferred whole-library load behind a "Loading…" popup on first entry.
  if (initialLoadPending) {
    GUI.drawPopup(renderer, tr(STR_LOADING));
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    if (SETTINGS.darkMode) renderer.invertScreen();
    loadLibraryKeys();
    initialLoadPending = false;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const char* headerTitle = atGroupList() ? headerLabel() : groups[selectedGroupIndex].c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect listRect{0, contentTop, pageWidth, contentHeight};

  const bool empty = atGroupList() ? groups.empty() : groupBookIdx.empty();
  if (empty) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
  } else if (atGroupList()) {
    GUI.drawList(
        renderer, listRect, groups.size(), selectorIndex, [this](int index) { return groups[index]; }, nullptr,
        [](int) { return UIIcon::Folder; },
        [this](int index) -> std::string {
          char buf[12];
          snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(groupCounts[index]));
          return std::string(buf);
        },
        false);
  } else {
    GUI.drawList(
        renderer, listRect, groupBookIdx.size(), selectorIndex,
        [this](int index) { return stemOf(bookPaths[groupBookIdx[index]]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(bookPaths[groupBookIdx[index]]); },
        [this](int index) -> std::string {
          const int progress = progressFor(bookPaths[groupBookIdx[index]]);
          if (progress >= 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", progress);
            return std::string(buf);
          }
          return "";
        },
        false);
  }

  const char* backLabel = atGroupList() ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = empty ? "" : tr(STR_OPEN);
  const auto labels =
      contextMenu.isOpen()
          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
          : mappedInput.mapLabels(backLabel, confirmLabel, empty ? "" : tr(STR_DIR_UP), empty ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  contextMenu.render(renderer);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
