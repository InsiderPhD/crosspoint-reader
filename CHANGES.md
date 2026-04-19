# CrossPoint Reader — Feature & Fix Summary

Changes made in this session relative to the `master` branch.

---

## New Features

### Auto-Turn: Seconds Per Page Mode
Previously auto-turn was measured in pages per minute (1, 3, 6, 12 PPM). It now uses direct seconds-per-page durations, making the setting more intuitive.

- **Options**: Off → Auto → 60s → 30s → 10s → 5s
- **Auto mode**: Uses your calibrated reading speed (`readingSpeedSecondsPerPage`). If no speed has been measured yet, Auto falls back to Off.
- **Menu display**: When Auto is selected, the menu shows the estimated time in parentheses, e.g. `Auto (~45s)`.

**Files changed**: `EpubReaderActivity.cpp`, `EpubReaderActivity.h`, `EpubReaderMenuActivity.h`, `EpubReaderMenuActivity.cpp`

---

### Mark as Completed (In-Reader Menu)
A new "Mark as Completed" option appears in the reader's menu. Selecting it:

1. Sets your progress to 100% in Recent Books.
2. Increments Books Finished in Reading Stats (if not already counted for this session).
3. Returns you to the Home screen.

**Files changed**: `EpubReaderActivity.cpp`, `EpubReaderMenuActivity.h`, `EpubReaderMenuActivity.cpp`, `english.yaml`

---

### Book Options Popup (Home Screen)
Long-pressing the Confirm button on a recent book on the Home screen now shows an inline popup modal instead of navigating to a separate screen.

- **Mark as Read**: Sets progress to 100% for that book.
- **Shelve Book**: Removes the book from your recents list (previously "Remove from Recents").
- **Dismiss**: Press Back to cancel with no changes.

The modal renders as an overlay on the current Home screen, with the book title, a divider, and the two selectable options. Input is correctly blocked to the main menu while the popup is visible. A button-bleed guard (`awaitingBookOptionsRelease`) prevents the long-press from immediately triggering an action.

**Files changed**: `HomeActivity.cpp`, `HomeActivity.h`, `english.yaml`

---

### Reading Stats — Home Menu Entry
Reading Stats is now accessible directly from the Home screen menu (previously it was buried elsewhere). The icon is a Book icon, positioned between File Transfer and Settings.

**Files changed**: `HomeActivity.cpp`, `HomeActivity.h`

---

### Reading Stats — Extended Metrics
The Reading Stats screen now shows 7 metrics instead of 3:

| Metric | Description |
|---|---|
| Reading Time | Total accumulated reading time |
| Pages Read | Total pages turned across all sessions |
| Books Finished | Count of completed books |
| Sessions | Number of reading sessions started |
| Avg. Session | Average session duration (Reading Time ÷ Sessions) |
| Reading Speed | Your calibrated seconds-per-page speed |
| In Progress | Number of books with 1–89% progress |

**Files changed**: `ReadingStatsActivity.cpp`, `ReadingStatsStore.h`, `ReadingStatsStore.cpp`, `english.yaml`

---

### File Browser — Progress Column
The file browser list now shows a right-aligned progress indicator for books that have been opened:

- **`X`** — Book is finished (≥90% progress)
- **`21%`** — Book is in progress (shows actual percentage)
- *(blank)* — Book has never been opened, or is a directory

The percentage is read from the in-memory Recent Books store (`RECENT_BOOKS.getBooks()`), so it only reflects books that have been opened since the firmware started or since last boot.

**Files changed**: `FileBrowserActivity.cpp`

---

### Session Tracking Across All Reader Types
Reading sessions are now tracked for `.epub`, `.txt`, and `.xtc` files. Each `onExit()` saves:

- Reading time (if session was ≥5 seconds)
- Page turns
- Books finished (if last page reached)
- Session count increment

Previously only EPUB tracked sessions.

**Files changed**: `TxtReaderActivity.cpp`, `XtcReaderActivity.cpp`, `ReadingStatsStore.h`, `ReadingStatsStore.cpp`

---

### Extra Small Font Size
A new **X-Small** font size option is available under Reader settings, adding a smaller size below the existing Small option.

- **Bookerly**: 10pt
- **NotoSans**: 8pt (no 10pt variant available; falls back to the existing 8pt UI font)
- **OpenDyslexic**: 8pt (no smaller variant available; same as Small)

**Files changed**: `CrossPointSettings.h`, `CrossPointSettings.cpp`, `main.cpp`, `fontIds.h`, `SettingsList.h`

---

### Inline Footnotes ("On page" mode)
Footnote text can now be rendered at the bottom of the page that references it, beneath a short horizontal rule — no need to open a menu.

A new **Footnotes** setting under Reader controls the behaviour:
- **On page** — footnote text appears inline at the bottom of the page it's referenced on
- **In menu** — existing 1.2 behaviour, footnotes accessible via the reader menu

Footnote body text is collected during EPUB indexing via a multi-phase scan of the chapter HTML (including cross-file footnote targets). Space is reserved during layout so the footnote block never overlaps page text, and a reference line is always kept on the same page as its footnote. Long footnote text wraps across multiple lines.

**Files changed**: `Page.cpp`, `Page.h`, `Section.cpp`, `Section.h`, `ChapterHtmlSlimParser.cpp`, `ChapterHtmlSlimParser.h`, `EpubReaderActivity.cpp`, `CrossPointSettings.h`, `SettingsList.h`, `english.yaml`

---

## Bug Fixes

### Shelving a Book Cleared All Recents
**Bug**: Removing a book from recents on the Home screen reset the entire list to empty.

**Cause**: After `recentBooks.clear()`, only `recentsLoaded = false` was set — `loadRecentBooks()` was never called. The next render triggered `loadRecentCovers()` on an empty list instead of reloading.

**Fix**: After removing a book, explicitly call `loadRecentBooks(metrics.homeRecentBooksCount)` before resetting state.

---

### Book Options Menu Appeared and Disappeared Immediately
**Bug**: The book options popup flashed on screen for ~1 frame and then vanished.

**Cause**: The Confirm button was held during the long-press (700ms), and the release event from that same press was consumed by the popup handler in the very next loop tick — immediately dismissing it.

**Fix**: `awaitingBookOptionsRelease` flag blocks the modal's input handling until the Confirm button is physically released after the long-press triggers.

---

### 99% Progress Not Counted as Finished
**Bug**: Closing a book at 99% didn't increment the Books Finished statistic.

**Cause**: The `onExit()` check used `>= 100` (or similar), which a book at 99% never reaches naturally.

**Fix**: The completion check in `EpubReaderActivity::onExit()` now uses `>= 90%` as the threshold for counting a book as finished, matching the "X" display logic used in the file browser. The `bookFinishedRecorded` guard prevents double-counting if the reader menu's "Mark as Completed" was already used in the same session.

---

### Dark Mode Missing from File Browser and Home Screen
**Bug**: The file browser and home screen did not apply the dark mode inversion before `displayBuffer()`.

**Fix**: Added `if (SETTINGS.darkMode) renderer.invertScreen();` before `renderer.displayBuffer()` in both `FileBrowserActivity::render()` and `HomeActivity::render()`.

---

## String / i18n Changes

| Key | Old value | New value |
|---|---|---|
| `STR_AUTO_TURN_PAGES_PER_MIN` | `"Auto Turn (Pages Per Minute)"` | `"Auto Turn (Seconds Per Page)"` |
| `STR_REMOVE_FROM_RECENTS` | `"Remove from Recents"` | `"Shelve Book"` |
| `STR_MARK_AS_READ` | *(new)* | `"Mark as Read"` |
| `STR_MARK_AS_COMPLETED` | *(new)* | `"Mark as Completed"` |
| `STR_BOOK_OPTIONS` | *(new)* | `"Book Options"` |
| `STR_READING_STATS` | *(new)* | `"Reading Stats"` |
| `STR_STATS_SESSIONS` | *(new)* | `"Sessions"` |
| `STR_STATS_AVG_SESSION` | *(new)* | `"Avg. Session"` |
| `STR_STATS_READING_SPEED` | *(new)* | `"Reading Speed"` |
| `STR_STATS_BOOKS_IN_PROGRESS` | *(new)* | `"In Progress"` |
| `STR_READING_SPEED` | *(new)* | `"Your Reading Speed"` |
| `STR_FOOTNOTE_ON_PAGE` | *(new)* | `"On page"` |
| `STR_FOOTNOTE_IN_MENU` | *(new)* | `"In menu"` |
