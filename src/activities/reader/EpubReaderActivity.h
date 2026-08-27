#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"
#include "util/DictionaryLookup.h"
#include "util/ProgressAutoSync.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long readingSpeedLastTurnMs = 0UL;
  unsigned long readingSessionStartMs = 0UL;
  uint32_t sessionPageTurns = 0;
  bool bookFinishedRecorded = false;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool autoPageTurnMode = false;  // True when using calibrated reading speed
  bool longPressFeedbackShown = false;
  bool longPressBackFired = false;
  bool longPressLeftFired = false;
  bool longPressRightFired = false;
  bool longPressPageBackFired = false;
  bool longPressPageForwardFired = false;
  bool showBookmarkMessage = false;
  bool bookmarkMessageWasRemoval = false;
  unsigned long bookmarkMessageTime = 0UL;

  // Transient dictionary popup ("Not found", "No dictionary set", a named
  // failure). Reuses the bookmark message's expiry timing rather than adding a
  // second popup lifetime to keep in step.
  bool showDictionaryMessage = false;
  StrId dictionaryMessage = StrId::STR_DICT_NOT_FOUND;
  unsigned long dictionaryMessageTime = 0UL;
  // Opened lazily on the first lookup and kept for the visit, so a second word
  // costs a lookup rather than another open + sidecar validate.
  DictionaryLookup dictionaryLookup;

  // In-memory copy of this book's bookmarks so the status-bar indicator can be
  // computed per render without touching the SD card. Reloaded on entry and
  // whenever a toggle or the bookmark list may have changed the file.
  std::vector<BookmarkEntry> cachedBookmarks;
  void reloadBookmarkCache();
  bool isCurrentPageBookmarked() const;

  enum class BookmarkToggleResult { None, Added, Removed };

  // Footnote support: taken from the page by move once its render is done, so the
  // footnotes menu can list them without holding the Page alive.
  std::vector<FootnoteEntry> currentPageFootnotes;
  // The status bar is drawn mid-render, before that move lands, so its footnote
  // indicator reads this instead of the vector.
  bool currentPageHasFootnotes = false;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Per-spine estimated page counts, built once on onEnter from cached section files.
  // Uncached spines are estimated via pages-per-byte from known spines.
  std::vector<uint16_t> spinePageCountCache;
  int cachedTotalBookPages = 0;

  void buildBookPageCache();
  // Book time-left in seconds: pages left in the book × persisted reading pace.
  // Uses the per-spine page cache when built, else the byte-fraction fallback.
  // Returns 0 when it can't estimate (no section/pace). Shared by the reader
  // menu and the onExit stash the sleep screen displays.
  uint32_t computeBookTimeLeftSeconds() const;

  // Renders `page` in place: the caller keeps ownership so it can move the page's
  // footnotes out afterwards instead of copying them before the render (see the
  // call site — the copy doubled peak heap on footnote-heavy pages).
  void renderContents(Page& page, int orientedMarginTop, int orientedMarginRight, int orientedMarginBottom,
                      int orientedMarginLeft);
  void renderStatusBar() const;
  // Footnote-return hint / button-hint strip; drawn even when the bar is hidden.
  void renderStatusBarTail(bool hintsActive) const;
  // Draws the optional button hints (themed front-button bar + Up/Down side boxes) showing
  // each button's mapped reader action, when SETTINGS.showButtonHints is enabled.
  void renderButtonHints() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  BookmarkToggleResult addBookmark();
  // Record reading-stats progress from the current section. MUST be called on the main task
  // only (never render()): see ActivityManager::isOnRenderTask. The SD write is debounced
  // inside READING_STATS.updateProgress().
  void recordStatsProgress();
  // Snapshot everything a background progress push needs while epub/section are
  // still alive. Main task only. Returns nullptr when the position can't be
  // built or the allocation fails.
  std::unique_ptr<ProgressAutoSync::Payload> buildAutosyncPayload(const ProgressAutoSync::Trigger& trigger);
  // Arm on a threshold crossing, report a fire on the 2nd page of a chapter.
  // Cheap and network-free: it decides *whether* to push, never pushes. Called
  // from pageTurn() (main task only — never from render()).
  bool autosyncDueThisTurn();
  // Blocking push. On a tight-heap board it releases the section and Epub first
  // and reloads them after; see ProgressAutoSync.h for why. Takes the reader
  // unresponsive for a few seconds either way.
  // Returns false when the reader was torn down (epub reload failed -> Home).
  bool runAutosyncNow();
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void toggleBluetoothFromReader();
  // Called from the idle loop: if Bluetooth is wanted but the stack is down (e.g.
  // a section build tore it out), and the page has settled, free the chapter
  // layout + Epub + glyph cache and re-enable the stack — the same heap-freeing
  // the manual toggle does, which is the only way to get the controller's
  // contiguous block back with a book open on X3. Silent; the page reloads from
  // cache.
  void maybeAutoRestoreBluetooth();
  // Reload the Epub released before a Bluetooth enable(). Failing here leaves
  // the reader with no book, so it bails to Home; returns false in that case
  // and the caller must return immediately.
  bool reloadEpubAfterBluetooth(const std::string& epubPath);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  // Drops the cached section so the current chapter re-paginates on the next render,
  // preserving the reading position. Used after a setting change (bionic, button hints)
  // that alters layout while staying in the reader.
  void reflowCurrentChapter();
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  // Dispatch a reader action (from the per-button settings) to its implementation.
  // Returns true if the action was terminal (i.e. the activity is exiting and the
  // caller should return immediately).
  bool executeReaderAction(CrossPointSettings::READER_ACTION action);

  // Clipping/highlight capture. startClipSelection builds the current page's word geometry and
  // opens the selection overlay; on confirm it persists the clipping and exports it. handleClippingJump
  // navigates to a saved clipping picked from the list. computeOrientedMargins returns the reading
  // viewport margins (shared with render() so the selection overlay uses the same text origin).
  // anchorX/anchorY (logical frame, -1 = unset): the Kindle-style tap-and-hold entry
  // point — the overlay opens with the word under that point selected and anchored.
  // forLookup swaps the range selection for a one-word pick feeding the
  // dictionary (see ClipSelectionActivity's singleWordMode). The word geometry
  // build is identical, which is the whole reason this is a mode rather than a
  // second copy of it.
  void startClipSelection(int anchorX = -1, int anchorY = -1, bool forLookup = false);
  // Open the word picker for a lookup, or show "No dictionary set" when none is
  // configured — there is nothing to pick a word for otherwise.
  void openDictionaryLookup();
  // Run the lookup for a picked word and open the definition viewer, or raise
  // the popup naming why it could not.
  void performDictionaryLookup(const std::string& word);
  void handleClippingJump(const ClippingJumpResult& jump);
  void computeOrientedMargins(int& orientedMarginTop, int& orientedMarginRight, int& orientedMarginBottom,
                              int& orientedMarginLeft) const;

  // Helper: open the reader menu (the full complex sub-activity launch).
  void openReaderMenu();

  // BookFusion sync — bidirectional auto-merge used by the long-press Confirm
  // "quick sync". The reader-menu Push/Pull entries use BookFusionSyncActivity
  // instead (full-screen flow), not this method.
  void performLongPressSync();
  void performKOReaderQuickSync();
  void performBookFusionSync();
  void connectWifiForSyncWithPopup(std::function<void()> onSuccess, const char* syncTitle = "BookFusion Sync");

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  // Dispatches tap zones and home-key actions itself (X4 Pro).
  bool consumesTouchInput() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
