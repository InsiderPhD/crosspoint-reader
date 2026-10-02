#include "HomeActivity.h"

#include <Bitmap.h>
#include <BluetoothHIDManager.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "../settings/BookFusionCoverRefreshActivity.h"
#include "../util/ConfirmationActivity.h"
#include "BookDetailsActivity.h"
#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "LibraryScan.h"
#include "MappedInputManager.h"
#include "ReadingStatsDetailActivity.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

// Largest-free-block floor below which arriving home triggers a defrag restart.
// See the checkpoint comment in onEnter() for how this value was chosen.
static constexpr size_t FRAG_RESTART_THRESHOLD = 24 * 1024;

int HomeActivity::getMenuItemCount() const {
  return 5 + getCoverSlotsUsed();  // Library, File Browser, File transfer, Stats, Settings
}

int HomeActivity::getCoverSlotsUsed() const { return static_cast<int>(recentBooks.size()); }

void HomeActivity::loadRecentBooks(int maxBooks) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  recentBooks.clear();
  recentBooks.reserve(maxBooks);
  homeSelectorIndex = 0;

  if (metrics.homePinnedBooks) loadPinnedBooks(maxBooks);
  const int pinnedCount = static_cast<int>(recentBooks.size());

  const auto& books = RECENT_BOOKS.getBooks();
  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    // A pinned book that is also a recent is already on the shelf.
    if (pinnedCount > 0 && RECENT_BOOKS.isPinned(book.path)) {
      continue;
    }

    if (recentBooks.size() == static_cast<size_t>(pinnedCount)) homeSelectorIndex = pinnedCount;
    recentBooks.push_back(book);
  }

  if (metrics.homeBackfillFromLibrary) backfillFromLibrary(maxBooks);
}

namespace {

// FNV-1a over the path: a stable rank for backfill picks, so the same library
// books stand on the shelf from one visit to the next.
uint32_t backfillRank(const std::string& path) {
  uint32_t h = 2166136261u;
  for (const char c : path) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  return h;
}

// Fills `out` from the book's existing metadata cache. False when there is
// none (a book never opened on this device): with buildIfMissing false the
// EPUB load fails on its single book.bin open instead of parsing the file.
bool loadCachedBookMeta(const std::string& path, RecentBook& out) {
  out.path = path;
  out.progressPercent = -1;
  out.pinned = false;
  if (FsHelpers::hasEpubExtension(path)) {
    Epub epub(path, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) return false;
    out.title = epub.getTitle();
    out.author = epub.getAuthor();
    out.coverBmpPath = epub.getThumbBmpPath();
    return true;
  }
  if (FsHelpers::hasXtcExtension(path)) {
    Xtc xtc(path, "/.crosspoint");
    if (!xtc.load()) return false;
    out.title = xtc.getTitle();
    out.author = xtc.getAuthor();
    out.coverBmpPath = xtc.getThumbBmpPath();
    return true;
  }
  return false;
}

// A filler for a book with no cache: its file name, no author, no cover (so
// the Bookshelf stands it spine-out, the way the Library lists such a book).
void fileNameBookMeta(const std::string& path, RecentBook& out) {
  out.path = path;
  out.progressPercent = -1;
  out.pinned = false;
  const auto slash = path.find_last_of('/');
  const auto dot = path.find_last_of('.');
  out.title = path.substr(slash + 1, dot == std::string::npos || dot < slash ? std::string::npos : dot - slash - 1);
  out.author.clear();
  out.coverBmpPath.clear();
}

}  // namespace

void HomeActivity::loadPinnedBooks(const int maxBooks) {
  const auto& recents = RECENT_BOOKS.getBooks();
  RecentBook book;
  for (const std::string& path : RECENT_BOOKS.getPinnedPaths()) {
    if (static_cast<int>(recentBooks.size()) >= maxBooks) break;
    if (!Storage.exists(path.c_str())) continue;  // pinned, then removed from the card
    const auto it =
        std::find_if(recents.begin(), recents.end(), [&path](const RecentBook& b) { return b.path == path; });
    if (it != recents.end()) {
      book = *it;  // keeps the recents' progress
    } else if (!loadCachedBookMeta(path, book)) {
      fileNameBookMeta(path, book);
    }
    book.pinned = true;
    recentBooks.push_back(book);
  }
}

// A shelf with gaps looks wrong, and the recents store only holds books that
// were opened here (10 at most), so after a few Shelve Books there is nothing
// left to show. Fill the remaining slots from the library instead:
//  - LibraryScan reuses its persisted index, so this is a file read plus one
//    directory stat each once the Library has been opened; on a card with no
//    index yet it is the one-off SD walk the Library would have done.
//  - Books already listed or shelved are skipped; the rest rank by a hash of
//    their path, so the picks (and their cached shelf covers) are stable.
//  - Metadata comes from each pick's cache. Books with a cache are preferred,
//    probing at most a few candidates per slot (each probe is a book.bin open:
//    ~20-25 ms on the X3); whatever is still short is filled by file name.
void HomeActivity::backfillFromLibrary(const int maxBooks) {
  const int need = maxBooks - static_cast<int>(recentBooks.size());
  if (need <= 0) return;

  std::vector<std::string> paths;
  LibraryScan::enumerateBooks(paths);
  if (paths.empty()) return;

  // (rank, index into paths) for every eligible book; 8 bytes each, sorted once.
  struct Candidate {
    uint32_t rank;
    uint32_t index;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(paths.size());
  for (size_t i = 0; i < paths.size(); ++i) {
    const std::string& path = paths[i];
    const bool listed =
        std::any_of(recentBooks.begin(), recentBooks.end(), [&path](const RecentBook& b) { return b.path == path; });
    if (listed || RECENT_BOOKS.isShelved(path)) continue;
    candidates.push_back({backfillRank(path), static_cast<uint32_t>(i)});
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });

  constexpr int kProbesPerSlot = 3;
  const int probeBudget = need * kProbesPerSlot;
  std::vector<uint32_t> uncached;  // candidates probed and found cacheless, in rank order
  uncached.reserve(probeBudget);

  recentBooks.reserve(maxBooks);
  int probes = 0;
  RecentBook book;
  for (const Candidate& c : candidates) {
    if (static_cast<int>(recentBooks.size()) >= maxBooks || probes >= probeBudget) break;
    ++probes;
    if (loadCachedBookMeta(paths[c.index], book)) {
      recentBooks.push_back(book);
    } else {
      uncached.push_back(c.index);
    }
  }
  for (size_t i = 0; i < uncached.size() && static_cast<int>(recentBooks.size()) < maxBooks; ++i) {
    fileNameBookMeta(paths[uncached[i]], book);
    recentBooks.push_back(book);
  }
  // Candidates past the probe budget, when every probed one was cacheless.
  for (size_t i = probes; i < candidates.size() && static_cast<int>(recentBooks.size()) < maxBooks; ++i) {
    fileNameBookMeta(paths[candidates[i].index], book);
    recentBooks.push_back(book);
  }
  LOG_DBG("HOME", "Backfilled %d shelf slot(s) from %zu library books (%d probed)", need, paths.size(), probes);
}

// Mirrors loadRecentCovers' conditions: true only when that pass would actually generate a thumb.
// BookFusion EPUBs are skipped there (API artwork only), so they must not force a second pass.
bool HomeActivity::anyRecentThumbNeedsGenerating(const int coverHeight) const {
  for (const RecentBook& book : recentBooks) {
    if (book.coverBmpPath.empty()) continue;
    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
    if (Storage.exists(coverPath.c_str())) continue;
    if (FsHelpers::hasEpubExtension(book.path)) {
      if (!BookFusionBookIdStore::hasBookId(book.path.c_str())) return true;
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      return true;
    }
  }
  return false;
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          // A BookFusion book's artwork MUST come from the API image cached at
          // download/refresh time. Their EPUBs carry unreliable embedded covers
          // -- often broken, often far smaller than the slot -- and generating
          // from one here would cache that bad image under the very thumb path
          // the themes read, with no way to tell it apart afterwards. Leave the
          // slot empty instead; the API path fills it (download, metadata
          // refresh, or the per-book Regenerate Cover action above). Deliberately
          // does NOT clear coverBmpPath: the path stays valid, the file is just
          // not there yet.
          if (BookFusionBookIdStore::hasBookId(book.path.c_str())) {
            LOG_DBG("HOME", "Skipping EPUB-derived thumb for BookFusion book %s", book.path.c_str());
            progress++;
            continue;
          }
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // Heap defragmentation checkpoint. Long reader sessions (32KB inflate blocks,
  // JPEG decode) and a BLE enable/disable cycle (freeing NimBLE returns ~48KB but
  // barely moves the largest block) fragment the heap in ways free() can't undo;
  // a silent reboot is the only real defrag. Home is the safe moment: no book
  // open, previous activity destroyed, nothing in flight. Fresh boot has ~33KB
  // largest block, so 24KB means "meaningfully degraded" — and also sits at the
  // floor of the 20-30KB zone where a later BLE controller init can hang.
  // Guards: skip while Bluetooth is wanted (enable state is per-session, so a
  // reboot would silently kill a connected remote), and skip in the first 20s
  // after boot so a device whose baseline is below the threshold can never
  // reboot-loop (the post-restart goHome always lands inside this window).
  if (millis() > 20000 && !BluetoothHIDManager::getInstance().isBluetoothWanted() &&
      !BluetoothHIDManager::getInstance().isEnabled()) {
    const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestBlock < FRAG_RESTART_THRESHOLD) {
      LOG_INF("HOME", "Heap fragmented (largest block %u < %u), silent restart to defrag",
              static_cast<unsigned>(largestBlock), static_cast<unsigned>(FRAG_RESTART_THRESHOLD));
      silentRestart();
      return;  // Not reached — ESP.restart() does not return — but keeps intent clear.
    }
  }

  loadRecentBooks(UITheme::getInstance().getMetrics().homeRecentBooksCount);
  selectorIndex = homeSelectorIndex;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // TEMPORARY DIAGNOSTIC — build with -DDISABLE_COVER_BUFFER=1 to skip the cover
  // snapshot entirely. The restore writes back byte-widened bounds (up to 7px
  // either side of the tile, holding whatever was adjacent at snapshot time), so
  // it is the prime suspect for a stray band that only appears on Home and only
  // clears on reboot. Returning false here makes Home redraw the cover normally:
  // slower, but with no direct framebuffer write at all.
#if DISABLE_COVER_BUFFER
  return false;
#endif
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::dispatchBookAction(BookContextMenu::Action action, const std::string& path,
                                      const std::string& title) {
  // Reload the recent-books vector after any mutation, keeping selector + cover
  // cache state in sync. Captures `this` to access members.
  auto reloadRecents = [this] {
    recentBooks.clear();
    loadRecentBooks(UITheme::getInstance().getMetrics().homeRecentBooksCount);
    selectorIndex = homeSelectorIndex;
    recentsLoaded = false;
    recentsLoading = false;
    coverRendered = false;
    coverBufferStored = false;
    requestUpdate();
  };

  switch (action) {
    case BookContextMenu::Action::MarkRead:
      RECENT_BOOKS.updateProgress(path, 100);
      RECENT_BOOKS.saveToFile();
      reloadRecents();
      break;
    case BookContextMenu::Action::ResetProgress:
      RECENT_BOOKS.updateProgress(path, -1);
      RECENT_BOOKS.saveToFile();
      reloadRecents();
      break;
    case BookContextMenu::Action::Pin:
      RECENT_BOOKS.togglePin(path);
      reloadRecents();
      break;
    case BookContextMenu::Action::Shelve:
      RECENT_BOOKS.shelveBook(path);
      reloadRecents();
      break;
    case BookContextMenu::Action::Reindex:
      if (FsHelpers::hasEpubExtension(path)) {
        // Delete only rendered sections — preserves progress, covers, and metadata.
        const std::string sectionsPath = Epub(path, "/.crosspoint").getCachePath() + "/sections";
        Storage.removeDir(sectionsPath.c_str());
      } else if (FsHelpers::hasXtcExtension(path)) {
        Xtc(path, "/.crosspoint").clearCache();
      }
      reloadRecents();
      break;
    case BookContextMenu::Action::Delete:
      startActivityForResult(std::make_unique<ConfirmationActivity>(
                                 renderer, mappedInput, tr(STR_DELETE_FROM_DEVICE) + std::string("?"), title),
                             [this, path, reloadRecents](const ActivityResult& res) mutable {
                               if (!res.isCancelled) {
                                 if (FsHelpers::hasEpubExtension(path)) {
                                   Epub(path, "/.crosspoint").clearCache();
                                 }
                                 Storage.remove(path.c_str());
                                 RECENT_BOOKS.removeBook(path);
                                 RECENT_BOOKS.saveToFile();
                                 reloadRecents();
                               }
                             });
      break;
    case BookContextMenu::Action::RegenerateCover:
      LOG_DBG("HAC", "Manual cover regeneration requested for: %s", path.c_str());
      if (FsHelpers::hasEpubExtension(path) && BookFusionBookIdStore::hasBookId(path.c_str())) {
        // BookFusion EPUB covers are unreliable; re-download the API cover instead.
        startActivityForResult(std::make_unique<BookFusionCoverRefreshActivity>(renderer, mappedInput, path, title),
                               [reloadRecents](const ActivityResult&) { reloadRecents(); });
        break;
      }
      if (FsHelpers::hasEpubExtension(path)) {
        Epub epub(path, "/.crosspoint");
        if (epub.load(false, true)) {  // buildIfMissing=false, skipLoadingCss=true
          const auto& metrics = UITheme::getInstance().getMetrics();
          const int coverHeight = metrics.homeCoverHeight;
          std::string coverPath226 = epub.getThumbBmpPath(coverHeight);
          if (Storage.exists(coverPath226.c_str())) {
            Storage.remove(coverPath226.c_str());
            LOG_DBG("HAC", "Removed existing cover: %s", coverPath226.c_str());
          }
          if (epub.generateThumbBmp(coverHeight)) {
            RECENT_BOOKS.updateBook(path, epub.getTitle(), epub.getAuthor(), coverPath226);
            LOG_DBG("HAC", "Cover regeneration successful: %s", coverPath226.c_str());
          } else {
            LOG_ERR("HAC", "Cover regeneration failed for: %s", path.c_str());
          }
        } else {
          LOG_ERR("HAC", "Failed to load EPUB metadata for cover regeneration: %s", path.c_str());
        }
      } else if (FsHelpers::hasXtcExtension(path)) {
        Xtc xtc(path, "/.crosspoint");
        if (xtc.load()) {
          const auto& metrics = UITheme::getInstance().getMetrics();
          const int coverHeight = metrics.homeCoverHeight;
          std::string coverPath = xtc.getThumbBmpPath(coverHeight);
          if (Storage.exists(coverPath.c_str())) {
            Storage.remove(coverPath.c_str());
            LOG_DBG("HAC", "Removed existing XTC cover: %s", coverPath.c_str());
          }
          if (xtc.generateThumbBmp(coverHeight)) {
            RECENT_BOOKS.updateBook(path, xtc.getTitle(), xtc.getAuthor(), coverPath);
            LOG_DBG("HAC", "XTC cover regeneration successful: %s", coverPath.c_str());
          } else {
            LOG_ERR("HAC", "XTC cover regeneration failed for: %s", path.c_str());
          }
        }
      }
      RECENT_BOOKS.saveToFile();
      reloadRecents();
      break;
    case BookContextMenu::Action::BookInfo:
      startActivityForResult(std::make_unique<BookDetailsActivity>(renderer, mappedInput, path, title,
                                                                   contextMenu.author(), contextMenu.progressPercent()),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    case BookContextMenu::Action::ViewStats:
      startActivityForResult(std::make_unique<ReadingStatsDetailActivity>(renderer, mappedInput, path),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
  }
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

  // Active modal — drive it and dispatch the chosen action (if any) on a terminal event.
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
      // Modal still up — selection navigation might have changed; redraw.
      requestUpdate();
    }
    return;
  }

#if FREEINK_DEVICE_X4PRO
  // Full Touch: a touch hold targets the tile under the finger — move the
  // selector there first so checkLongPress below (which reads the same touch
  // event and suppresses the contact) opens the menu for that book.
  if (CrossPointSettings::FULL_TOUCH_UI) {
    int lx, ly;
    if (mappedInput.wasTouchLongPressPoint(lx, ly)) {
      const int tile = tileIndexAt(lx, ly);
      if (tile >= 0 && tile < static_cast<int>(recentBooks.size())) {
        selectorIndex = tile;
      }
    }
  }
#endif

  // Long-press Confirm on a recent-book tile opens the book options modal.
  if (selectorIndex < static_cast<int>(recentBooks.size())) {
    const auto& book = recentBooks[selectorIndex];
    if (contextMenu.checkLongPress(mappedInput, book.path, book.title, book.author, book.progressPercent)) {
      // Surface tags in the popup when the cached metadata has them (existing cache only,
      // no rebuild, so opening the menu never blocks).
      if (FsHelpers::hasEpubExtension(book.path)) {
        Epub epub(book.path, "/.crosspoint");
        if (epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
          contextMenu.setInfoTags(epub.getTags());
        }
      }
      requestUpdate();
      return;
    }
  }

#if FREEINK_DEVICE_X4PRO
  // Full Touch: horizontal swipes step through the cover slots, wrapping — the
  // carousel gesture, and useful on any multi-cover home.
  //
  // A leftward swipe arrives as Back, because that is the only Back the X4 Pro
  // has (TouchListNav::tabSwipeNext declines to repurpose it for that reason).
  // Home is the exception: it is the root screen, so Back closes nothing here
  // and its hint label is already blank. The context-menu branch above returns
  // before this point, so the menu keeps its own Back.
  if (CrossPointSettings::FULL_TOUCH_UI) {
    if (mappedInput.wasSwipe() == MappedInputManager::Swipe::Right && rotateCoverSelection(1)) return;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && rotateCoverSelection(-1)) return;
  }

  // Full Touch: first tap on a tile moves the selector, a second tap opens it.
  if (CrossPointSettings::FULL_TOUCH_UI) {
    int lx, ly;
    if (mappedInput.wasTapPoint(lx, ly)) {
      const int tile = tileIndexAt(lx, ly);
      if (tile >= 0) {
        const bool activate = TouchListNav::tapActivates(tile, selectorIndex);
        selectorIndex = tile;
        if (activate) {
          activateSelectedTile();
        } else {
          requestUpdate();
        }
        return;
      }
      // Dead space (header, gaps): no action.
    }
  }
#endif

  // One handler per direction so a theme with a 2-D home (Bookshelf's grid)
  // can move by rows; -1 from the theme keeps the linear next/previous step
  // (Down/Right forward, Up/Left back) every other theme uses.
  const auto navigate = [this, menuCount](const BaseTheme::NavDirection dir, const bool forward) {
    const int target = GUI.homeNavigate(selectorIndex, getCoverSlotsUsed(), menuCount, dir);
    if (target >= 0 && target < menuCount) {
      if (target == selectorIndex) return;  // edge of the grid: nothing to redraw
      selectorIndex = target;
    } else {
      selectorIndex = forward ? ButtonNavigator::nextIndex(selectorIndex, menuCount)
                              : ButtonNavigator::previousIndex(selectorIndex, menuCount);
    }
    requestUpdate();
  };
  // Static: built once, not a heap vector per direction on every loop pass.
  using Button = MappedInputManager::Button;
  static const std::vector<Button> kDown{Button::Down};
  static const std::vector<Button> kRight{Button::Right};
  static const std::vector<Button> kUp{Button::Up};
  static const std::vector<Button> kLeft{Button::Left};
  buttonNavigator.onPressAndContinuous(kDown, [&] { navigate(BaseTheme::NavDirection::Down, true); });
  buttonNavigator.onPressAndContinuous(kRight, [&] { navigate(BaseTheme::NavDirection::Right, true); });
  buttonNavigator.onPressAndContinuous(kUp, [&] { navigate(BaseTheme::NavDirection::Up, false); });
  buttonNavigator.onPressAndContinuous(kLeft, [&] { navigate(BaseTheme::NavDirection::Left, false); });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // If this release was the menu's confirmation/dismissal, the helper has
    // already handled it — skip short-press handling.
    if (contextMenu.consumeLongPressFlag()) return;
    activateSelectedTile();
  }
}

#if FREEINK_DEVICE_X4PRO
bool HomeActivity::rotateCoverSelection(const int delta) {
  const int coverCount = static_cast<int>(recentBooks.size());
  if (coverCount <= 1) return false;

  if (selectorIndex >= coverCount) {
    // Coming from the icon row: enter at the end the rotation is heading away
    // from, so the first step shows a cover rather than skipping one.
    selectorIndex = delta > 0 ? 0 : coverCount - 1;
  } else {
    selectorIndex = (selectorIndex + delta % coverCount + coverCount) % coverCount;
  }
  requestUpdate();
  return true;
}
#endif

Rect HomeActivity::coverStripRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight};
}

Rect HomeActivity::menuRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{
      0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, renderer.getScreenWidth(),
      renderer.getScreenHeight() -
          (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 + metrics.buttonHintsHeight)};
}

int HomeActivity::tileIndexAt(const int lx, const int ly) const {
  const int coverSlotsUsed = getCoverSlotsUsed();
  const int coverSlot = GUI.hitTestRecentBookCover(coverStripRect(), coverSlotsUsed, lx, ly);
  if (coverSlot >= 0) {
    return coverSlot;
  }
  const int menuCount = getMenuItemCount() - coverSlotsUsed;
  const int menuSlot = GUI.hitTestButtonMenu(menuRect(), menuCount, lx, ly);
  return menuSlot >= 0 ? coverSlotsUsed + menuSlot : -1;
}

void HomeActivity::activateSelectedTile() {
  {
    const int coverSlotsUsed = getCoverSlotsUsed();

    // Calculate dynamic indices based on which options are available
    int idx = 0;
    int menuSelectedIndex = selectorIndex - coverSlotsUsed;
    const int libraryIdx = idx++;
    const int fileBrowserIdx = idx++;
    const int fileTransferIdx = idx++;
    const int statsIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else if (menuSelectedIndex == libraryIdx) {
      onLibraryOpen();
    } else if (menuSelectedIndex == fileBrowserIdx) {
      onFileBrowserOpen();
    } else if (menuSelectedIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (menuSelectedIndex == statsIdx) {
      onStatsOpen();
    } else if (menuSelectedIndex == settingsIdx) {
      onSettingsOpen();
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, coverStripRect(), recentBooks, selectorIndex, coverRendered, coverBufferStored,
                          bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_LIBRARY), tr(STR_BROWSE_FILES), tr(STR_FILE_TRANSFER),
                                        tr(STR_READING_STATS), tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Library, Folder, Transfer, Stats, Settings};

  const int menuOffset = getCoverSlotsUsed();
  GUI.drawButtonMenu(
      renderer, menuRect(), static_cast<int>(menuItems.size()), selectorIndex - menuOffset,
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = contextMenu.isOpen()
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  contextMenu.render(renderer);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    // Only a missing thumb needs the second pass (loadRecentCovers below). With every thumb
    // already cached it would repaint identical pixels: one wasted FAST refresh per Home entry.
    if (anyRecentThumbNeedsGenerating(metrics.homeCoverHeight)) {
      requestUpdate();
    } else {
      recentsLoaded = true;
    }
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() {
  // "Tags"/"Authors" views swap the SD-directory browser for a whole-library grouped view.
  switch (SETTINGS.folderView) {
    case CrossPointSettings::FOLDER_VIEW_TAGS:
      activityManager.goToTagBrowser();
      break;
    case CrossPointSettings::FOLDER_VIEW_AUTHORS:
      activityManager.goToAuthorBrowser();
      break;
    case CrossPointSettings::FOLDER_VIEW_SERIES:
      activityManager.goToSeriesBrowser();
      break;
    default:
      activityManager.goToFileBrowser();
      break;
  }
}

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onStatsOpen() { activityManager.goToStats(); }
