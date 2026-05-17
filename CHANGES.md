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
- **NotoSans**: 8pt (falls back to the existing small UI font)
- **OpenDyslexic**: 6pt (new dedicated variant)

**Files changed**: `CrossPointSettings.h`, `CrossPointSettings.cpp`, `main.cpp`, `fontIds.h`, `SettingsList.h`

---

### Monospace Reader Font (JetBrains Mono)
A new **Monospace** option in Reader → Font Family, backed by JetBrains Mono. Useful for code-heavy EPUBs (technical books, programming references) where a fixed-width font keeps indentation, ASCII art, and tabular text readable.

Bundled sizes: 6, 8, 10, 12pt in regular and bold. Generated with the existing `fontconvert.py` pipeline using `--2bit --compress --pnum` flags (anti-aliased, compressed, proportional numerals), matching the other reader fonts.

The source TTFs ship in `lib/EpdFont/builtinFonts/source/JetbrainsMono/` so the conversion is reproducible.

**Files added**: 8 × `mono_<size>_<style>.h`, JetBrains Mono TTFs

**Files changed**: `all.h` (includes), `convert-builtin-fonts.sh` (Mono conversion loop), `fontIds.h` (regenerated)

---

### Inline Footnotes ("On page" mode)
Footnote text can now be rendered at the bottom of the page that references it, beneath a short horizontal rule — no need to open a menu.

A new **Footnotes** setting under Reader controls the behaviour:
- **On page** — footnote text appears inline at the bottom of the page it's referenced on
- **In menu** — existing 1.2 behaviour, footnotes accessible via the reader menu

Footnote body text is collected during EPUB indexing via a multi-phase scan of the chapter HTML (including cross-file footnote targets). Space is reserved during layout so the footnote block never overlaps page text, and a reference line is always kept on the same page as its footnote. Long footnote text wraps across multiple lines.

**Files changed**: `Page.cpp`, `Page.h`, `Section.cpp`, `Section.h`, `ChapterHtmlSlimParser.cpp`, `ChapterHtmlSlimParser.h`, `EpubReaderActivity.cpp`, `CrossPointSettings.h`, `SettingsList.h`, `english.yaml`

---

### Library View (3xN Cover Grid)
A paginated grid view of every book on the SD card, accessible by switching to the new **Lyra Library** UI theme (Settings → Display → UI Theme). The grid lays out 3 covers per page in landscape and 6 (3×2) in portrait, with the cover size and styling matched to the Lyra3Covers home tile — cropped bitmap fill, rounded selection corners with light-gray dither sides, title (2 lines, wrapped) + author (truncated) + reading progress below each cover.

**Navigation**:
- Up/Down/Left/Right: step one cover, wraps at edges.
- Long-press a direction: jump to the next/previous page.
- Confirm: open the selected book.
- Long-press Confirm: open the book options menu (mark read, reset progress, shelve, delete, reindex, regenerate cover) — same six-option modal as the home recents tile.
- Back: return to home.

**Loading behaviour**: on first visit, books with no cached thumbnail show a placeholder + a centered "Loading…" popup. Covers fill in one per render pass (~3–5 s each on EPUBs that have never been opened); subsequent visits are instant since `thumb_226.bmp` is cached on the SD card. Once all covers on a page are processed the popup disappears — any remaining placeholders represent books without an extractable cover image. Generation is confined to the render task to avoid a `TaskPriorityDisinherit` mutex race when both tasks decode bitmaps simultaneously.

**Lyra Library theme**: based on Lyra3Covers, the third home tile is permanently a "Library" button (Library icon + label centered inside the cover frame) that launches the grid view. Selecting it from the Settings theme picker is how users opt in to the library.

**Other niceties**:
- Header bar styled to match Recent Books (Lyra header at the top).
- Page indicator (`X / Y`) centered at the bottom of the grid.
- Lyra-style scrollbar on the right edge when there's more than one page.
- BookFusion-synced books get a `& ` title prefix, matching the marker used by the home recents tile.
- Recursive SD enumeration via BFS deque, so books in any subdirectory are picked up.
- Newest-added sort comes for free from FAT enumeration order (reversed).

**Supporting refactor**: the long-press book-options modal was extracted from `HomeActivity` into a shared `BookContextMenu` helper so the same six-option behaviour drives both the home and library screens — fewer places to keep in sync.

**Files added**: `LibraryActivity.cpp`, `LibraryActivity.h`, `LyraLibraryTheme.cpp`, `LyraLibraryTheme.h`, `BookContextMenu.cpp`, `BookContextMenu.h`

**Files changed**: `ActivityManager.cpp`, `ActivityManager.h`, `HomeActivity.h`, `HomeActivity.cpp`, `UITheme.cpp`, `UITheme.h`, `BaseTheme.h`, `CrossPointSettings.h`, `SettingsList.h`, `english.yaml`

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
| `STR_THEME_LYRA_LIBRARY` | *(new)* | `"Lyra Library"` |
| `STR_VIEW_LIBRARY` | *(new)* | `"View Library"` |
| `STR_LIBRARY` | *(new)* | `"Library"` |
| `STR_NO_BOOKS_IN_LIBRARY` | *(new)* | `"No books found"` |
| `STR_LONG_PRESS_ACTION` | *(new)* | `"Long-press Confirm Action"` |
| `STR_REFRESH_SCREEN` | *(new)* | `"Refresh Screen"` |
| `STR_SYNC_WITH_BOOKFUSION` | *(new)* | `"Sync with BookFusion"` |
| `STR_BF_*` (30+ keys) | *(new)* | BookFusion UI strings — see `english.yaml` |

---

### BookFusion Integration

CrossPoint Reader now syncs with [BookFusion](https://www.bookfusion.com), a cloud reading platform. Users can link their BookFusion account, browse and download their library directly to the device, and sync reading progress bidirectionally.

**Account linking**:
- OAuth 2.0 device code flow — device displays a verification URL plus a short user code, the user authorises on bookfusion.com
- QR code rendered alongside the URL on the auth screen for easy mobile linking
- Encrypted access token persisted at `/.crosspoint/bookfusion.json`
- Settings → BookFusion Sync → Link Account / Unlink Account

**Library browsing & download**:
- Browse by category: Currently Reading / Favorites / Plan to Read / Completed / All Books (5 tabs)
- Paginated list (8 books per page) with a "Load next page…" sentinel
- Selecting a book streams the EPUB to the SD card root as a sanitised `Title - Author.epub`
- A sidecar `/.crosspoint/bookfusion_<md5>.json` maps the local file → BookFusion book_id
- Cover thumbnail is pre-generated **during** the "Downloading…" screen (after the network transfer, before the "Download Complete" popup) so the home screen shows the cover with no first-render lag
- "Download Complete" popup shows the freshly-generated cover thumbnail above the title

**Reading progress sync**:
- Long-press Confirm inside a book triggers a bidirectional sync (fetch remote position, push current position) when the **Long-press Confirm Action** setting is set to BookFusion sync
- Default setting remains a full e-ink refresh; sync is opt-in
- After a long-press action, the release event no longer opens the reader menu (previously every long-press also fired the menu on release)
- Auto-link: uploading an EPUB via the web server attempts to match it to a BookFusion book by title+author across the first 5 pages of the user's library, and writes the sidecar automatically on a hit

**Visual indicators**:
- `& ` prefix in book lists (Recent Books on Home, File Browser, Library) for any book whose local file has a BookFusion sidecar — a visual cue that this book syncs with the cloud

**Image / cover work to support iPad-sized art**:
- BookFusion covers are typically 1200×1800 to 3000×4500 (iPad-class); the JPEG converter now uses JPEGDEC's native scaled-decode flags (`JPEG_SCALE_HALF/_QUARTER/_EIGHTH`) to cut decode time and MCU buffer size by up to 16×
- Memory-budget bound moved post-scale, so large covers that previously hit the 2048×3072 raw bound are scaled down first and accepted
- Sleep-screen Cover mode now renders the open book's cover for BookFusion EPUBs the same as any local EPUB; added diagnostic logs along the cover path for easier debugging when it falls back

**Files added**:
- `lib/BookFusionSync/` — `BookFusionSyncClient` (HTTP + OAuth + progress + search + download URL), `BookFusionTokenStore` (encrypted token), `BookFusionBookIdStore` (per-EPUB sidecars). Reuses `lib/KOReaderSync/ProgressMapper` for the BookFusion ↔ CrossPoint position conversion.
- `src/activities/settings/BookFusionAuthActivity.{h,cpp}` — device code flow UI
- `src/activities/settings/BookFusionBrowserActivity.{h,cpp}` — library browser + download
- `src/activities/settings/BookFusionSettingsActivity.{h,cpp}` — settings panel

**Files changed**:
- `CrossPointSettings.h`, `SettingsList.h`, `JsonSettingsIO.{h,cpp}` — `longPressAction` setting; token persistence
- `SettingsActivity.{h,cpp}` — BookFusion Sync entry in settings menu
- `EpubReaderActivity.{h,cpp}` — long-press sync; release-after-long-press no longer opens the menu
- `HomeActivity.cpp`, `FileBrowserActivity.cpp`, `Lyra3CoversTheme.cpp`, `LyraTheme.cpp` — `& ` prefix on BF-linked titles
- `SleepActivity.cpp` — diagnostic logs along the cover sleep fallback paths
- `JpegToBmpConverter.cpp` — native scaled-decode + post-scale memory bound
- `CrossPointWebServer.cpp`, `CrossPointWebServerActivity.cpp` — auto-link uploaded EPUBs to BookFusion library
- `HttpDownloader.cpp` — used for downloading BF EPUBs from presigned URLs
- `main.cpp` — load BookFusion token at boot
- `english.yaml` — 30+ new `STR_BF_*` keys + the long-press action strings

---

### Lyra Library Theme + Library View

A new **Lyra Library** UI theme adds a "View Library" tile to the home screen, and a full library grid activity showing all books on the SD card.

**Home screen (`Lyra Library` theme)**:
- Shows up to 2 recent books as cover tiles (same as Lyra Extended)
- The 3rd tile is always a "View Library" shortcut with a library icon
- Selecting it navigates to the Library activity
- Same cover rendering and selection highlight style as Lyra Extended

**Library activity**:
- Scans the SD card root for all EPUB and XTC files (up to 60 books)
- Displays 6 books per page in a 3×2 grid
- Cover thumbnails loaded from SD cache; books with no cached cover show a placeholder
- After first render, missing thumbnails for the current page are generated in the background (same popup mechanic as the home screen)
- Cover buffer (48 KB) is stored after rendering a page, so within-page navigation (no SD re-reads) is fast
- Page navigation via Up/Down side buttons; book selection via Left/Right front buttons; Confirm to open; Back to return home
- Page indicator shown when more than one page of books exists
- Cover height adapts to orientation: 180 px portrait, 100 px landscape

**Architecture changes**:
- `ThemeMetrics` gains a `hasLibraryTile` bool field (zero-initialised → `false` for all existing themes)
- `HomeActivity` respects `hasLibraryTile`: loads `homeRecentBooksCount` books (2 for Lyra Library), adds the library tile as a selectable item, and routes Confirm to `goToLibrary()` when that tile is active
- `CrossPointSettings::UI_THEME` gains `LYRA_LIBRARY = 3`
- `ActivityManager` gains `goToLibrary()`

**Files changed**: `BaseTheme.h`, `CrossPointSettings.h`, `UITheme.cpp`, `HomeActivity.cpp`, `HomeActivity.h`, `ActivityManager.h`, `ActivityManager.cpp`, `SettingsList.h`, `english.yaml`

**Files added**: `LyraLibraryTheme.h`, `LyraLibraryTheme.cpp`, `LibraryActivity.h`, `LibraryActivity.cpp`

---

## Performance Improvements

### Footnote Processing Optimization
Footnote processing performance has been significantly improved when the **Footnotes** setting is set to "In menu" mode:

- **Conditional Processing**: Cross-file footnote scanning now only runs when footnotes are set to "On page" mode
- **Skip Heavy Operations**: When footnotes are in menu mode, the expensive multi-phase scan of linked files is bypassed entirely
- **Faster Chapter Loading**: Books with extensive footnote cross-references load noticeably faster in menu mode
- **Better Link Detection**: Improved filtering to exclude chapter navigation links that were incorrectly detected as footnotes

This optimization particularly benefits books with many footnotes or complex cross-file footnote structures.

**Files changed**: `ChapterHtmlSlimParser.cpp`, `ChapterHtmlSlimParser.h`

---

### Font Memory Optimization
The built-in font library has been streamlined to reduce memory usage and binary size:

**Removed font variants**:
- **Bold-Italic styles**: All bold-italic combinations removed from Bookerly, NotoSans, and OpenDyslexic
- **Large sizes**: 18pt variants removed for Bookerly and NotoSans families
- **Style reduction**: Font styles limited to Regular, Italic, and Bold only

**Current font sizes**:
- **Bookerly**: 10pt (X-Small), 12pt (Small), 14pt (Medium), 16pt (Large)  
- **NotoSans**: 12pt (Small), 14pt (Medium), 16pt (Large)
- **OpenDyslexic**: 6pt (X-Small), 8pt, 10pt, 12pt (Small), 14pt (Medium+)

This optimization saves significant flash storage and reduces the risk of memory fragmentation during font loading.

**Files changed**: `convert-builtin-fonts.sh`, `all.h`, various `*_bolditalic.h` files (deleted), font size arrays in build scripts
