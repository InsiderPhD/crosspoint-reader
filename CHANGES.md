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

### Reading Stats — Full System Overhaul
Reading Stats was rebuilt from a small counter screen into a multi-page analytics system with a dedicated data model, per-book/day aggregation, and resilient date handling for X4 deep-sleep clock drift.

**UI overhaul (4 pages)**:
- **Overview**: streak, max streak, daily goal progress, total reading time, books finished, books started, and annual reading chart.
- **Started Books**: paginated list of in-progress books with title/author, reading time, and progress; short-press Confirm opens details; long-press Confirm removes the selected stats entry.
- **Weekly**: last 7 days vs 30 days totals, average day, days read, goal days, best day, plus daily bar chart.
- **Monthly**: month summary (month total, days read, best day, year total) plus calendar-style heatmap and legend.

**Store/data overhaul**:
- Added normalized `ReadingBookStats` + `ReadingDayStats` structures and summary caching for quick reads of today/7-day/30-day/streak metrics.
- Session lifecycle now tracks active reading with heartbeat/deferred-save behavior, per-session snapshots, and capped session logs.
- Added retention pruning and aggregation rebuild paths so stale day buckets are removed and totals stay consistent.

**Clock resilience for X4**:
- Date-sensitive stats use a reference timestamp fallback chain: authoritative/NTP-synced time, then last known valid app timestamp, then latest known book timestamp, then latest recorded day ordinal.
- Invalid clocks are ignored for day attribution, avoiding corrupted streak/day data when waking from deep sleep with an unset RTC.

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
Reading sessions are now tracked for `.epub`, `.txt`, and `.xtc` files through the unified Reading Stats store.

- Reading time accrues continuously while the session is active.
- Session logs/session-count increments are recorded for substantial sessions (minimum duration threshold in the stats store).
- Completion/progress updates are persisted through the same shared path.

Previously only EPUB had end-to-end session tracking.

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

---

## Sort Order & Navigation Improvements

### Sortable File & Book Lists
Library, Recent Books, and File Browser now support 10 sort orders, switchable on the fly via a Sort Menu popup. The current sort label is shown on the right side of each list's header (e.g. `Library    Name A-Z`).

**Sort modes**:
- Name A-Z / Name Z-A
- Author A-Z / Author Z-A
- Recently opened / Least recently opened
- Most read / Least read (by reading progress)
- Newest on device / Oldest on device (by SD card file modtime)

**Opening the menu**: short-press the Power button anywhere in a list activity. A modal pops up with all 10 options; the currently active sort is marked with a `•`. Confirm picks, Back or a second Power press cancels.

**Persistence**: the last-picked sort is saved to `settings.json` (`sortMode` field) and propagates to every list activity and across reboots. One global preference, not per-list. The setting entry is hidden from the Settings UI — it's edited only via the in-activity Sort Menu.

**Folder handling (File Browser)**: folders always pin to the bottom of the list in natural-sort order, regardless of the current sort mode. Only files are reordered by the selected mode. Non-book files (TXT, BMP) with no progress / author metadata sink to the end of the file group for sorts that don't apply to them.

**Lazy metadata loading (Library / File Browser)**: Author and Date-Added sorts require per-book metadata that isn't kept resident in those activities. The first time one of those modes is selected per activity session, a "Sorting…" popup appears while the cache is built (one `book.bin` parse + one SD stat per book). Subsequent re-sorts use the cache and are instant. Recents has all metadata already loaded so no extra pass is needed.

**Memory footprint** (per-activity, freed on exit):
- Resident: 1 byte for the mode + ~50 byte SortMenu state + 2 × N bytes for the sorted-index permutation.
- Worst case after both lazy caches populated: ~21 KB for a 500-book Library (4 × N for the date-added cache + 24 × N + heap overflow for the author cache).
- Transient peak during a sort call: 24 × N for the `SortEntry` vector, dropped immediately after.

**Files added**: `src/sorting/SortMode.{h,cpp}`, `src/components/SortMenu.{h,cpp}`

**Files changed**:
- `lib/hal/HalStorage.{h,cpp}` — new `HalFile::getModifyDateTime` / `getModifyDateTimePacked` passthroughs to SdFat's `FsFile`, used for the Date-Added sort modes.
- `src/activities/home/LibraryActivity.{h,cpp}` — removed the old `SortDirection` enum (subsumed by `SortMode`); added SortMenu wiring, lazy `authorCache` and `dateAddedCache`, and a deferred "Sorting…" popup before the metadata pass.
- `src/activities/home/RecentBooksActivity.{h,cpp}` — wired SortMenu, replaced the pre-existing "Apply sorting to the paths" TODO with `rebuildSortedIndices()`, indirected all `recentBooks[]` accesses through the sort permutation.
- `src/activities/home/FileBrowserActivity.{h,cpp}` — split the directory load into `fileEntries` + `folderEntries`, sort only files, materialise `files` as `[…sorted fileEntries, …folderEntries]` so folders pin to the bottom.
- `src/CrossPointSettings.h` — new `uint8_t sortMode = 0` (defaults to `SortMode::AlphabeticAsc`).
- `src/SettingsList.h` — persistence-only entry for `sortMode` (no category → hidden from UI but round-tripped through `settings.json`).
- `lib/I18n/translations/english.yaml` — `STR_SORT_BY`, `STR_SORTING`, 10 × `STR_SORT_<mode>` labels.

---

### Global Long-Press Back → Home
Holding the Back button for 1 second from any activity now returns to the Home screen. Fires once per hold cycle (resets on release) so a continued hold doesn't keep retriggering.

The File Browser's old "long-press Back goes to the SD root folder" gesture is removed — superseded by the global behaviour. The lock that ignores a held-Back-on-entry (`lockLongPressBack`) is preserved for the case where you enter the file browser with Back held from a previous activity.

Reader activities (`EpubReaderActivity`, `TxtReaderActivity`, `XtcReaderActivity`) still have their own long-press-Back-home handlers; those are now redundant (the global handler fires first) but left in place to keep reader code untouched.

**Files changed**: `src/main.cpp` (new global handler before `activityManager.loop()`), `src/activities/home/FileBrowserActivity.cpp` (removed the per-activity gesture).

---

### Short-Press Power → Sort Menu; Power Setting Scoped to Reader
The **Short Power Button Click** setting (`Settings → Controls`) used to apply globally. Its behaviour is now scoped to the reader only: Page Turn / Sleep / Force Refresh fire only while reading a book. Outside the reader (Library / Recents / File Browser), short-press Power opens the new Sort Menu instead.

Removed the **Ignore** option from this setting — it's no longer reachable in the settings UI. The enum was renumbered (`PAGE_TURN=0, SLEEP=1, FORCE_REFRESH=2`), and the default flipped from `IGNORE` to `PAGE_TURN`. Existing saved values that used the old `PAGE_TURN=2` will load as `FORCE_REFRESH` after upgrade — one-time re-pick required.

The Force Refresh dispatch was moved out of the global main loop into a new `ReaderUtils::detectAndApplyForceRefresh` helper, called from each of the three reader activities. PAGE_TURN was already reader-scoped via `ReaderUtils::detectPageTurn` / `XtcReaderActivity`.

**Files changed**: `src/main.cpp` (removed the global Force Refresh handler), `src/activities/reader/ReaderUtils.h` (new `detectAndApplyForceRefresh` helper), `src/activities/reader/EpubReaderActivity.cpp`, `src/activities/reader/TxtReaderActivity.cpp`, `src/activities/reader/XtcReaderActivity.cpp` (each calls the helper once per loop), `src/CrossPointSettings.h` (enum renumber + default flip), `src/SettingsList.h` (removed `STR_IGNORE` from the option list).

---

### LyraLibrary Tile Label
The third tile on the home screen under the Lyra Library theme used to be labelled **Library** (both inside the cover frame and as the title below). Both labels now read **Library** to make it clearer the tile is a shortcut to the full grid, not a settings entry.

**Files changed**: `src/components/themes/lyra/LyraLibraryTheme.cpp` (both `tr(STR_LIBRARY)` references → `tr(STR_VIEW_ALL_COVERS)`), `lib/I18n/translations/english.yaml` (new `STR_VIEW_ALL_COVERS` key).

---

### Reading Stats — Icon Updates

The Overview and Weekly stats pages now use more descriptive icons:

| Metric | Old icon | New icon |
|---|---|---|
| Max Goal Streak (Overview) | Streak flame | Confetti |
| Daily Goal (Overview) | Book | Checkbox |
| Daily Goal (Weekly) | Check mark | Checkbox |

Icons are 24×24 1bpp, generated from Tabler SVGs via `scripts/convert_icon.py`.

**Files added**: `src/components/icons/confetti24.h`, `src/components/icons/checkbox24.h`

**Files changed**: `src/activities/home/ReadingStatsActivity.cpp`

---

### Bug Fixes (this session)

#### Reading Stats — Pagination and Monthly Navigation
**Bug**: Directional navigation was inconsistent after the multi-page stats overhaul. Side buttons (Up/Down) did not reliably page on Overview/Weekly/Monthly, and Monthly short-press actions could page-switch instead of month-switch.

**Cause**: The `loop()` input handler had a single branch for all pages that mapped short-press directional inputs to `changePage()`, ignoring per-page navigation semantics. The Monthly page was never given its own short-press handler for `changeViewedMonth()`.

**Fix**: Rewrote the `loop()` input decision tree. Long-press on any direction always changes stats page. Short-press on Monthly now calls `changeViewedMonth(-1/+1)` (left/up = previous month, right/down = next month), while non-Monthly/non-Started-Books pages use short directional presses for page switching.

**Files changed**: `src/activities/home/ReadingStatsActivity.cpp`

---

#### "Back Twice to Exit" in Library / Recents / File Browser
**Bug**: After interacting with the Sort Menu in any list activity (even just opening and dismissing it), the next Back press was silently swallowed — users had to press Back twice to actually exit the activity.

**Cause**: `SortMenu::consumeCloseFlag()` set a one-shot flag whenever the menu closed via any release, and the activity's Back handler checked + cleared it. The flag persisted until *some future* Back press, which might be much later and unrelated to the menu interaction — at which point it would swallow an innocent exit gesture.

**Fix**: Removed the close-flag mechanism entirely. It was unnecessary because each activity already returns unconditionally inside its `if (sortMenu.isOpen()) { …; return; }` block, so the release that closes the menu can't fall through to the activity's normal handler in the same frame.

**Files changed**: `src/components/SortMenu.{h,cpp}`, `src/activities/home/LibraryActivity.cpp`, `src/activities/home/RecentBooksActivity.cpp`, `src/activities/home/FileBrowserActivity.cpp`.

---

### String / i18n Changes (this session)

| Key | Value |
|---|---|
| `STR_SORT_BY` | `"Sort by"` |
| `STR_SORT_ALPHA_ASC` | `"Name A-Z"` |
| `STR_SORT_ALPHA_DESC` | `"Name Z-A"` |
| `STR_SORT_AUTHOR_ASC` | `"Author A-Z"` |
| `STR_SORT_AUTHOR_DESC` | `"Author Z-A"` |
| `STR_SORT_LAST_OPENED_NEW` | `"Recently opened"` |
| `STR_SORT_LAST_OPENED_OLD` | `"Least recently opened"` |
| `STR_SORT_PROGRESS_MOST` | `"Most read"` |
| `STR_SORT_PROGRESS_LEAST` | `"Least read"` |
| `STR_SORT_DATE_ADDED_NEW` | `"Newest on device"` |
| `STR_SORT_DATE_ADDED_OLD` | `"Oldest on device"` |
| `STR_SORTING` | `"Sorting…"` |
| `STR_VIEW_ALL_COVERS` | `"Library"` |
| `STR_IGNORE` | *(removed from Short Power Button settings options; key kept in `english.yaml` for legacy)* |

The legacy `STR_SORT_ALPHABETICAL` / `STR_SORT_RECENT` / `STR_SORT_PROGRESS` keys (left over from an unfinished `SortingManager` sketch on `master`) were removed in favour of the new `STR_SORT_<mode>` set.
