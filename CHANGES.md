# CrossPoint Reader — Feature & Fix Summary

A running technical log of what this fork adds on top of upstream CrossPoint, newest first.

---

## Unreleased

### The X4 Pro action bar stops offering "Select"

Full Touch turned the bottom hint strip into real tap targets, and every menu
inherited the button boards' pair: **Back | Select**. On a screen where the rows
themselves are tappable, that second slot ran exactly the handler a tap on the
highlighted row already runs — a button whose only job was to duplicate the list
it sat under.

The bar now leaves it out. `Activity::tapActivatesConfirm()` defaults to
`handlesDirectTouch()`, which is precisely the set of screens that hit-test taps
against their own drawn UI (first tap moves the cursor, second tap activates),
and `ActivityManager` publishes it to `ActionBar` before each render, so no
screen threads a flag through its render path. A plain menu is left with one
full-width **Back**; Home, which has no Back, shows no bar at all. Screens that
opted into all four slots keep their Left/Right actions and just lose the middle
one — the Bluetooth device list still offers Disconnect and Retry.

The exception is the screens where Confirm has no on-screen equivalent: the date
and number spinners (manual date, session date edit, reading date selection,
book reading adjustment) step the highlighted field on a second tap and commit
only from the bar, and clip selection uses taps to move the word range. Those
five override the virtual back to false. Getting that wrong strands the user —
this board has no front buttons, and `handlesDirectTouch()` has already switched
off the tap-anywhere-is-Confirm injection — so the rule is documented at the
virtual itself.

X4 Pro Full Touch only; the button boards still label all four front buttons.

**Files changed**: `src/activities/Activity.h`, `src/activities/ActivityManager.cpp`,
`src/components/ActionBar.{h,cpp}`, the five opt-out activity headers.

### A line of text is one heap allocation, not five vectors

A resident page holds roughly 25-30 laid-out lines, and each one used to carry five parallel containers — `std::vector<std::string>` words plus per-word x-positions, styles and the two bionic arrays. That is on the order of **250 small heap blocks per page load**, and it was the single largest driver of fragmentation on the C3. Fragmentation is the failure that matters here: free bytes can look healthy while the section builder still cannot find the 32KB contiguous window miniz needs, which is exactly the state a resident BLE stack puts the heap in.

`TextBlock` now takes **one** allocation — an offset table plus a NUL-terminated text blob, with typed views bound over it. Words come back as `const char*` and go straight to `drawText` with no `std::string` materialised on the way. The two bionic arrays are omitted from the arena entirely when no word on the line has a split, so bionic reading costs zero per-word RAM when it is off. The arena is taken with `makeUniqueNoThrow`, so an OOM produces an invalid block the caller drops rather than an `abort()` under `-fno-exceptions`.

Two smaller changes ride along on the same path:

- **Page elements move from `shared_ptr` to `unique_ptr`.** A block is produced by the layout and handed to exactly one `PageLine`; the atomic refcount and per-object control block bought nothing on a single-core RISC-V part.
- **`ParsedText`'s word list becomes a `std::deque`.** A CJK paragraph splits every character, and a vector of a few thousand `std::string`s reallocates its whole element array into one 64-128KB contiguous block — precisely the request that fails on a fragmented heap. A deque grows in ~512B nodes, so the largest request stays ~2KB regardless of token count.

Separately, `Epub::load()` now **releases the resolved CSS rule map** once loading finishes. It is only needed while section caches are being built, and `Section::createSectionFile()` reloads it on demand; holding it pinned tens of KB for a whole reading session, worst on a warm resume into an already-cached chapter where the builder never runs and so never cleared it.

Section cache format **v41**. Ported from upstream crosspoint #2547 and #2814.

**Files changed**: `lib/Epub/Epub/blocks/TextBlock.*`, `lib/Epub/Epub/{Page,ParsedText,Section}.*`, `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.*`, `lib/Epub/Epub.cpp`, `src/activities/reader/EpubReaderActivity.cpp`.

### Bluetooth asks for a restart instead of taking one

The BLE controller wants one contiguous block of at least 30KB. Once a session has been torn down, small allocations left scattered through the working region cap the largest block far below that — measured on-device at ~17KB largest with ~82KB *free*. Nothing the firmware can free fixes that; only a fresh heap does. By the time `enable()` is called from the reader the chapter layout, the `Epub` and the glyph cache have already been handed back, so there is genuinely nothing left to give up.

The reader briefly answered that by saving progress and rebooting itself, once per boot. That is now gone: a memory refusal puts up **"Restart the device to free memory"** and leaves the decision with the reader. Restarting a device out from under someone mid-page costs them the thing they were doing for a mechanism they never asked for, and it does it blind — a fresh heap is not guaranteed to be enough either, so the reboot can spend the interruption and still land on the same refusal.

`RTC_NOINIT` flag, the once-per-boot guard and the boot-time re-arm are removed with it. The standing "Bluetooth wanted" flag is unaffected: it is persisted in settings, so a paired remote still comes back by itself once a chapter is resident.

**Files changed**: `src/SilentRestart.h`, `src/main.cpp`, `src/activities/reader/EpubReaderActivity.cpp`, `lib/I18n/translations/english.yaml`.

### A heap attribution ladder

`HeapReport::logBrief()` is a one-line probe — free, largest block, live block count — cheap enough to sit at lifecycle transitions rather than only behind the POWER chord's full region map. Tagged readings at `boot.done`, `reader.enter`, `reader.entered` and `reader.section` turn a single serial capture into a delta per subsystem.

`capture()` also reports **allocated and free block counts** now. That is the number that moves when an allocation's *shape* changes: collapsing several per-object vectors into one arena can leave free bytes identical while halving the live block count, and so the fragmentation pressure. Reading it on the same page of the same book before and after a change is the most direct evidence available on-device.

**Files changed**: `src/util/HeapReport.*`, `src/main.cpp`, `src/activities/reader/EpubReaderActivity.cpp`.

### Pre-release firmware channel (Dev tab)

*Settings -> Check for updates* reads GitHub's `/releases/latest`, which is defined to skip prereleases — that is how stable devices are kept off beta and X4 Pro builds, and it stays the default. The new **Enable Pre-Releases** toggle switches the check to the release **list** and offers the newest entry carrying this build's asset, so a C3 falls through an x4pro-only beta rather than being handed an image it cannot boot.

The toggle sits on the **Dev** tab and is read gated on `devMode` as well as its own value, so leaving Dev Mode cannot strand a device on the beta channel with no visible switch. (Value settings tagged Dev now render on that tab at all, which they previously did not.)

`ReleaseJsonParser` gained a `releaseList` mode for the array shape: it walks the array in GitHub's order (newest `created_at` first), discards the partial state of any release without a matching asset, and locks in the first that has one. An empty or asset-less list reports `NO_UPDATE` rather than a parse error, because that is a normal outcome for this channel and not a malformed payload. Note `isUpdateNewer()` is unchanged and still correct semver, so `1.7.9-rc1` is **not** newer than `v1.7.9`.

**Files changed**: `lib/JsonParser/ReleaseJsonParser.*`, `src/network/OtaUpdater.cpp`, `src/{CrossPointSettings,SettingsList}.h`, `src/activities/settings/SettingsActivity.cpp`, `test/release_json_parser/ReleaseJsonParserTest.cpp`, `lib/I18n/translations/english.yaml`.

### Libby: covers, a nameable renewal failure, and a date that persists

- **Loans get their artwork.** Library books are Adobe-encrypted, so the cover inside the EPUB is not dependably readable and loans sat in the library as blank tiles. A loan's `id` *is* the OverDrive title id, so the largest published cover is fetched on demand from OverDrive's public catalogue — no chip, no card, no `Authorization` header — rather than every loan in the list carrying a ~128 byte URL for the one title about to be sent. Best-effort: a failure logs and the send still succeeds.
- **"Already fulfilled by another user" is its own error.** ADEPT's `E_LIC_ALREADY_FULFILLED_BY_ANOTHER_USER` is the one refusal the user can act on, and retrying will never clear it, so it no longer reads as a generic server error. The renewal path also reports the two-part outcome — renewed on Libby, but the copy on the card kept its old date.
- **A loan opens against the date the device already knows.** Enforcement used `isClockValid()`, so a boot whose NTP sync did not land refused to open a protected book even though the device had learned the date on an earlier boot and persisted it. `TimeUtils::getBestKnownTimestamp()` returns the live clock when valid and the last persisted sync otherwise, and reports 0 only for a device that has *never* known the date — which still fails closed, because an unsynced cold boot sits near epoch 0 and would read as before any due date. The fallback is a lower bound on real time, so a loan that expired since the last sync opens until the device next learns the date.

**Files changed**: `lib/Libby/{LibbyClient,AdeptClient}.*`, `src/activities/settings/LibbyBrowserActivity.*`, `lib/Epub/ContentProtection.cpp`, `src/util/TimeUtils.*`, `lib/I18n/translations/english.yaml`.

### Protected library loans (Libby)

Adds a **decrypt-on-read** path for protected EPUBs, and a **Libby** row in **File Transfer** that uses it. Books stay encrypted at rest: entries are decoded in memory, streamed in chunks, and never written back to the card.

The split is the point. Everything scheme-specific — linking an account, listing loans, the fulfilment handshake and all of its crypto — runs as JavaScript in the user's browser (`src/network/html/LibbyPage.html`, `js/libbyCrypto.js`, WebCrypto). The device contributes only the three things a browser page physically cannot do for itself:

- **relay** — make an outbound HTTPS call (the browser is blocked by CORS)
- **fetch** — stream a URL straight to SD, never through RAM
- **write** — persist a small credential or rights sidecar to SD

The consequence worth stating: this costs **zero resident RAM**. There is no Libby object, no cached session and no background task. The handlers exist only while the web server does, and the server only exists inside `CrossPointWebServerActivity`, which reboots on the way out.

Three memory decisions carry the feature on a C3:

- **miniz takes its entire inflate state in ONE ~44KB allocation** (the 32KB dictionary the deflate format mandates, plus Huffman tables), and that single request is what fails — opening a 143MB protected comic, free heap was 61,720 bytes while the largest free block was 21,492. Fragmentation, not exhaustion. The SDK's new `InflateAllocator` seam lets the firmware serve it from the region an `InflateScratchLease` already lends (the idle framebuffer, contiguous since boot). uzlib and miniz never inflate at the same time, so they share the one region rather than each claiming the framebuffer.
- **Only the READ half of the `Crypto` interface is linked** — no `WOLFSSL_KEY_GEN`, no `WC_RC2`, since activation happens in the browser. `NO_WOLFSSL_ESP32_CRYPT_RSA_PRI` forces software RSA: the Espressif wolfSSL port hardcodes `ESP_HW_RSAMAX_BIT` at 4096 while the C3's MPI peripheral stops at 3072 and returns `MP_EXPTMOD_E` rather than falling back. One RSA-1024 private op per book open, so the software path costs nothing measurable.
- **`HalStorage::readFileToString()` probes the largest free block before resizing**, because `std::string` growth is a bare allocation under `-fno-exceptions`.

A protected book that cannot be opened now **explains itself** instead of bouncing silently back to the browser: expired loan, unset clock (with an offer to go and set it over Wi-Fi), device not authorised, or the crypto layer's own wording for a rights/key failure.

The File Transfer menu's rows are now addressed through a table rather than by index arithmetic — with two conditional rows (BookFusion pre-link, Libby pre-link) the old truncate-the-list approach would put the labels and the selection out of step.

**Files added**: `lib/Epub/ContentProtection.cpp`, `lib/Libby/{LibbyClient,AdeptClient}.*`, `src/activities/settings/LibbyBrowserActivity.*`, `src/network/html/LibbyPage.html`, `src/network/html/js/libbyCrypto.js`, `scripts/test_libby_crypto.mjs`.
**Files changed**: `platformio.ini`, `lib/Epub/Epub.*`, `lib/InflateReader/InflateReader.*`, `lib/hal/HalStorage.*`, `src/network/CrossPointWebServer.*`, `src/activities/network/{NetworkModeSelectionActivity,CrossPointWebServerActivity}.*`, `src/activities/reader/ReaderActivity.*`, `lib/I18n/translations/english.yaml`. SDK: `libs/book/ContentProtection/` (injectable inflate allocator, PKCS#12 cert fallback).

### A session can hand the 48KB framebuffer back to the heap

E-ink is bistable: once a screen has been refreshed the panel holds that image with no buffer behind it. For a session that paints its final screen and then stops drawing, the framebuffer is 48KB of pure dead weight — on this board the difference between roughly 20KB and 68KB free.

`GfxRenderer::releaseFrameBuffer()` gives it back and `isRenderable()` says whether drawing is still legal. The release is deliberately **one-way**: there is no realloc wrapper, because reallocating relocates the 48KB and progressively fragments a heap with no PSRAM behind it. Callers must therefore be sessions that restart on exit.

The web server is the first (and so far only) caller. Without those 48KB it starts near 20KB free, and a few page-load fetches take it low enough that lwIP cannot get pbufs and a single TCP write inside a response stalls for tens of seconds — long enough to trip the loop watchdog mid-response. It also buys the contiguous block a wolfSSL handshake needs, which is what the Libby page's relay depends on. `CrossPointWebServerActivity` already reboots on the way out, which is what makes a one-way release acceptable.

Everything that can draw from the main loop is gated: the render task drops requests centrally in `ActivityManager` (so the waiter notification still fires and `requestUpdateAndWait()` cannot deadlock), and the two POWER chords plus the serial `SCREENSHOT` command carry their own checks because they write straight to the buffer.

**Files changed**: `lib/GfxRenderer/GfxRenderer.*`, `lib/hal/HalDisplay.*`, `src/activities/ActivityManager.cpp`, `src/main.cpp`, `src/activities/network/CrossPointWebServerActivity.cpp`.

### Pages no longer lose words with a Bluetooth remote connected

With the BLE stack resident, free heap sits near 10KB and the font cache's per-page ~3KB group-decompression buffer stops fitting. The glyphs in the group that missed then paint as blanks — whole words vanishing from an otherwise normal page, only with a page-turner connected.

Two changes, both aimed at that one allocation:

- **The group scratch is now persistent and shared.** `prewarmCache()` used to `malloc`/`free` a temp buffer per group; it now takes the largest group's worth once, up front — largest block first, before the page buffers carve up the heap — and reuses the same grow-only block `getBitmap()`'s fallback path already keeps. `clearPageCache()` releases the per-page glyph buffers between pages but deliberately holds the scratch, because handing back the biggest contiguous block only to ask for it again is exactly what fails here. `clearCache()` still frees everything for the heap-critical paths (section builds, TLS).
- **The page retries once when it still doesn't fit.** `allocFailures` in the cache stats is ground truth — the prewarm has just reported that this page *will* paint incomplete — so the reader pauses the BLE stack (~50KB), defers its auto-restore so the ~5s retry can't re-enter the same tight heap, and lays the page out again. One retry only; a second failure means the heap is short for another reason and looping would just stall the page turn.

Also fixes a latent out-of-bounds read: `getGroupIndex()` returns a sentinel for glyphs no group owns, which the prewarm then used to index `fontData->groups`.

**Files changed**: `lib/EpdFont/FontDecompressor.*`, `lib/GfxRenderer/FontCacheManager.*`, `lib/hal/BluetoothHIDManager.h`, `src/activities/reader/EpubReaderActivity.cpp`.

### Reader Controls: Sleep and Mark Finished retired

Two bindable reader actions are gone from **Settings → Reader Controls**. **Sleep** (8) was redundant — a long press of **Power** is hard-wired to sleep on every board, so binding a second control to it only cost a slot. **Mark Finished** (14) survives as **Mark as Read** in the Book Options popup, which is where the rest of the per-book actions already live.

Enum values 8 and 14 stay **reserved** rather than being renumbered: `settings.json` stores the raw number, so reusing them would silently repoint every existing binding above the gap. `CrossPointSettings::isRetiredReaderAction()` keeps the picker from offering them, and `sanitizeReaderActions()` (run from `JsonSettingsIO::loadSettings`) rewrites any slot still holding one — or any out-of-range value — to `READER_ACTION_NONE`, so a settings file from an older firmware can't leave a button bound to an action that no longer dispatches. The legacy `longPressAction` / `shortPwrBtn` migration now maps its Sleep cases to None for the same reason.

**Files changed**: `src/CrossPointSettings.{h,cpp}`, `src/JsonSettingsIO.cpp`, `src/activities/settings/ReaderControlsActivity.cpp`, `src/activities/reader/{EpubReaderActivity,XtcReaderActivity}.cpp`, `lib/I18n/translations/english.yaml`, `README.md`, `USER_GUIDE.md`.

### Dictionary lookup

Offline **StarDict** word lookup while reading, backported from upstream CrossPoint. Dictionaries live in `/dictionaries/<folder>/` (or the hidden `/.dictionaries/`) on the SD card and are selected in **Settings → Reader → Dictionary**; the row and the reader-menu **Look Up** row both hide themselves when nothing usable is installed. `READER_ACTION_DICTIONARY` (24) makes Look Up bindable to any button, tap zone or hold.

Reads `.idx` + `.dict`/`.dict.dz` with optional `.syn` synonyms, binary-searching lazily built `.qidx`/`.sidx` sampled-offset sidecars so no index is held in RAM. A miss retries dictionary synonyms then mini stemming. `sametypesequence=h` dictionaries lay out through `ChapterHtmlSlimParser` for real headings/bold/italics/lists, falling back to `htmlToPlainText` whenever the heap gate declines.

Three fork-specific decisions:

- **No second word selector.** Upstream ships `DictionaryWordSelectActivity`; we already had a better one in `ClipSelectionActivity` (pooled 14-byte `WordRef`s, cross-page cursor, tap hit-testing). It gained a `singleWordMode` flag that returns a `WordPickResult` instead of running `ClipTextBuilder`, so there is still exactly one word selector to keep working.
- **No `initWithRing()` backport.** Upstream hands the inflate reader a caller-owned 32KB window. Our `InflateReader::init(true)` already prefers an outstanding `InflateScratchLease` before it mallocs, so `DictionaryLookup::run()` just holds the framebuffer lease across the index build and the lookup — the contiguous window comes from the framebuffer, which fragmentation cannot reach.
- **Bluetooth and lookups are mutually exclusive on the C3.** The lease covers the inflate window only; the `.dz` chunk table, the definition buffer and the styled page set still compete with the ~50KB the BLE stack holds. `openDictionaryLookup()` refuses up front with "Turn off Bluetooth to look up words" rather than failing after the user has picked a word. Deliberately *not* a `BleMemoryPause` — that would drop a paired page-turner mid-sentence. Fenced by `CROSSPOINT_BLE_EXCLUSIVE`, so the X4 Pro is unaffected.

No cache-format change: nothing here touches `book.bin` or `section.bin`, so existing `.crosspoint` caches survive the update.

**Files added**: `src/util/{DictZip,Dictionary,DictionaryRegistry,DictHtmlPages,DictionaryLookup,HtmlToPlainText}.*`, `src/activities/reader/DictionaryDefinitionActivity.*`, `src/activities/settings/DictionarySelectActivity.*`, `docs/dictionary.md`, `test/html_to_plain_text/` + `test/run_html_to_plain_text_test.sh`.
**Files changed**: `src/CrossPointSettings.h`, `src/SettingsList.h`, `src/activities/ActivityResult.h`, `src/activities/reader/{ClipSelectionActivity,EpubReaderActivity,EpubReaderMenuActivity}.*`, `src/activities/settings/{SettingsActivity,ReaderControlsActivity}.*`, `src/util/StringUtils.h`, `lib/I18n/translations/english.yaml`.

### X4 Pro support (experimental)

Initial port to the **Xteink X4 Pro** — an ESP32-S3 board, so it ships as a **separate binary** (`pio run -e x4pro`) rather than as an addition to the C3 image. `freeink-sdk`'s `BoardConfig.h` refuses to link devices from two MCU families, so `[env:x4pro]` unsets `FREEINK_DEVICE_X3`/`FREEINK_DEVICE_X4` and the RISC-V-only `WOLFSSL_SP_RISCV32`, and switches SD to the native SDMMC block-device interface.

The board has no front buttons, so `MappedInputManager` synthesizes the four front roles from screen swipes (left = Back, right = Confirm, up/down = Up/Down) and deliberately bypasses `SETTINGS.frontButton*` — a stale or imported remap can never make Back or Confirm unreachable. The two side keys are Left/Right in menus and PageBack/PageForward in the readers. The home-pad hold is hard-wired to the reader menu so the menu (and its Go Home row) always stays reachable.

Also: deep-sleep rail shutdown (`powerDownRailsForSleep`) for the gated GT911/SD rails, and PWM frontlight support.

**Files changed**: `platformio.ini`, `src/MappedInputManager.*`, `lib/hal/HalPowerManager.*`, `lib/hal/HalGPIO.*`, `lib/hal/HalDisplay.*`, plus device fences across the activity layer.

### Full Touch mode

Opt-in tap hit-testing for the X4 Pro (**Settings → System → Full Touch Mode**, off by default). With it off, a tap is a Confirm on the current selection. With it on, `TouchListNav` dispatches taps against the real drawn geometry: tap an unselected row to move the cursor, tap the selected row to activate. Vertical swipes page whole lists, a right swipe steps tabs, a left swipe is Back — and on tabbed screens Back steps back through the tabs before closing.

The list rect, item count and subtitle flag passed to `TouchListNav::tapRow()` must match the activity's `drawList()` call, or hit-testing and rendering disagree. Activities that consume touch themselves override `handlesDirectTouch()`.

Readers are unaffected: tap zones (left/middle/right thirds), hold zones and the home pad keep their own configurable actions, all bindable from Reader Controls.

**Files changed**: `src/util/TouchListNav.h`, `src/components/UITheme.*`, `src/components/themes/*`, most of `src/activities/settings/`, `src/activities/home/`, `src/activities/reader/`.

### Frontlight

Brightness (0–100%, 10% steps) and, on warm/cool boards, a warmth mix that splits the total between the two LED strings at constant brightness. Exposed in **Settings → Display** and as in-place cycling rows in the reader menu, so the light can be changed without leaving the page. Fenced behind `FREEINK_CAP_FRONTLIGHT` / `FREEINK_CAP_WARMLIGHT`, so the JSON keys don't round-trip on hardware that can never drive them.

**Files changed**: `lib/hal/HalFrontlight.*`, `src/SettingsList.h`, `src/activities/reader/EpubReaderMenuActivity.*`, `platformio.ini` (FrontlightManager dep).

### Per-element status bar

The status bar's show/hide toggles and Book/Chapter enums are replaced by one position picker per element: **Hide → Left → Center → Right**. Battery, book title, chapter title, book %, chapter page count, book/chapter time-left, clock, a new **bookmark indicator** (drawn only on bookmarked pages) and a new **Bluetooth indicator** (Down / Up / Connected) can each sit in any cluster.

Added alongside: a **Top Margin** setting (0–20 px, 4 px steps) between body text and the bar, and a **Hide Status Bar** reader action. Hiding keeps the bar's strip reserved during layout so the section cache stays valid — the reader skips drawing it and re-centres the page by redistributing the margin at draw time.

`migrateStatusBarPositions()` derives the new fields from the legacy toggles the first time a settings file without them is loaded.

**Files changed**: `src/CrossPointSettings.*`, `src/SettingsList.h`, `src/JsonSettingsIO.cpp`, `src/activities/settings/StatusBarSettingsActivity.cpp`, `src/components/themes/BaseTheme.*`, `src/components/icons/bookmark.h`, `src/components/icons/bluetooth*.h`.

### Reader menu revamp

Two-line summary at the top: reading speed, chapter position + time left, book % + time left on line one; clock, date, an "autosync off" notice when the boot NTP check failed, and today's reading against the daily goal on line two.

**Reading Speed** and **Mark as Read** rows are gone (speed now lives in the summary; Mark Finished is a bindable reader action and a Book Options entry). Dark Mode, Button Hints, Orientation and Frontlight cycle **in place** without closing the menu. Sync rows are resolved per book via `ProgressAutoSync::providerFor()` and labelled for the backend that will actually handle them (*Sync: Push to BookFusion*, *Pull from KOReader*, …), appearing only when a provider exists. Clippings / Bookmarks / Bluetooth / Sync groups can each be hidden from **Settings → Reader**.

**Files changed**: `src/activities/reader/EpubReaderMenuActivity.*`, `src/activities/reader/EpubReaderActivity.*`, `lib/I18n/translations/english.yaml`.

### BLE page turner: two-button mapping and auto-restore

The press detector learns each remote's report *shape* rather than decoding HID keycodes, and now tracks up to three shapes per device (multi-function remotes send more than one; sharing one idle mask between them made the detector go blind). Idle is anchored on quiescence — the frame before a gap in the traffic is the one the report came to rest on — which stops a learning window that closes mid-hold from learning a *pressed* frame as idle and inverting every signature for the session.

On top of that: a **mapping wizard** (each step captures the same button twice, which is what exposes a one-button toggle remote that alternates codes), so a mapped remote gets **page back** as well as forward, while unmapped remotes keep the any-button-pages-forward behaviour. Stale mappings are detected against the learned idle frame and surfaced as a **Re-map** prompt. First-time setup is guided end to end: enable → scan → pair → map → live press test.

**Bluetooth now restores itself.** `maybeAutoRestoreBluetooth()` brings the stack back once the reader is idle with a chapter resident, a few seconds past the last page turn, and past a WiFi settle gate — the same heap the manual toggle frees, freed the same way. Progress Autosync therefore no longer costs the remote for the rest of the session.

**Files changed**: `lib/hal/BluetoothHIDManager.*`, `src/activities/settings/BluetoothSettingsActivity.*`, `src/activities/reader/EpubReaderActivity.cpp`, `src/CrossPointSettings.h`.

---

## 1.7.6 — July 2026

### Section builds no longer fail on a fragmented heap

Metadata rebuilds and `loadEpub()` now lend the display framebuffer to inflate as its dictionary window, so a chapter build that needs a ~32KB contiguous block can find one even when free heap is plentiful but fragmented.

### Wake from deep sleep on a short power press

The 400ms hold gate (an anti-pocket-wake measure tied to the legacy `shortPwrBtn` field) made waking feel unresponsive, and the field it keyed on is no longer what the Reader Controls UI writes. A short press now always wakes.

**Files changed**: `src/activities/reader/EpubReaderActivity.cpp`, `lib/Epub/`, `src/CrossPointSettings.h`

---

## 1.7.3 – 1.7.5 — June–July 2026

*(1.7.3 and 1.7.4 were mid-stream version bumps, not separate releases; everything below shipped in 1.7.5.)*

### Bluetooth page-turner remotes

NimBLE-Arduino is **vendored** in `lib/NimBLE-Arduino` with RAM patches (halved mbuf pools, 4KB host stack, trimmed controller config) — the framework's `sdkconfig.h` hard-defines `CONFIG_BT_NIMBLE_*`, so command-line flags alone don't work. Pair a cheap BLE HID clicker and any button turns the page forward; the press detector learns what "idle" looks like for that model instead of decoding keycodes. Bluetooth settings gained a live press test and a reworked UI, and all of its text is translated.

Heap is the binding constraint: image decoding is skipped while the BLE stack is up, and the stack is torn down whenever the RAM is needed elsewhere.

### TLS migrated from mbedTLS to wolfSSL

Outbound HTTPS (BookFusion, KOReader, OPDS, OTA, fonts) now runs through `SecureNet`/wolfSSL. The precompiled mbedTLS transport needed two ~16.7KB **contiguous** record buffers plus a P-256 bignum spike — the X3 sync OOM. wolfSSL pins the TLS 1.3 key_share to X25519 (fixed 32-byte field math, no bignum temporaries) and uses the RISC-V single-precision backend.

### Framebuffer leases

TLS sessions, JPEG decode, OPDS/URL transfers and OTA downloads borrow the display framebuffer as scratch instead of allocating. Downloads are "quiet": a frozen info card stays on screen rather than repainting mid-transfer, which also removes a class of ghosting.

### Library, sorting, clippings and stats

Library browsing and sorting improvements; clippings (highlights) ported from the CrossInk fork; reading-stats fixes plus sleep-screen stats; the recent-books activity removed in favour of the new browse/library flows; sleep screens full-refresh so they don't ghost on battery.

### SDK migrated to freeink-sdk

`open-x4-epaper/community-sdk` → `Free-Ink/freeink-sdk`, an MIT re-architecture. Include paths and class names are preserved by a compat shim, so firmware source was unchanged; the new required dependency is `BoardConfig` (device profiles carrying geometry, pins and waveforms).

**Files changed**: `platformio.ini`, `lib/NimBLE-Arduino/`, `lib/hal/BluetoothHIDManager.*`, `src/network/`, `freeink-sdk/`

---

## 1.7.2 — June 2026

Maintenance re-release of 1.7.1 to ensure the OTA update is delivered reliably (version bumped so devices reliably detect it as newer). No functional or code changes from 1.7.1.

**Files changed**: `platformio.ini` (version bump)

---

## 1.7.1 — June 2026

### Reliable BookFusion / OPDS downloads on low heap
Large downloads (notably image-heavy BookFusion EPUBs) were failing with `writeToStream error: -8` (`HTTPC_ERROR_TOO_LESS_RAM`). `HTTPClient::writeToStream` mallocs a contiguous 4 KB read buffer internally, which can't be satisfied when this activity runs at ~16 KB free with a fragmented heap. `HttpDownloader::downloadToFile` now streams the body itself through a single reused 1 KB buffer, decoding identity, chunked, and connection-close framings. A chunked transfer that ends without its terminating zero-length chunk is now rejected as truncated rather than written as a corrupt (partial-JPEG) file. Covers all callers: BookFusion, OPDS/Calibre, OTA, fonts.

**Files changed**: `HttpDownloader.cpp`

### Large-book download warning
Pressing download on a BookFusion EPUB of 10 MB or more now shows a confirm screen with the title, size, and a warning that image-heavy books may be slow or unstable — Confirm downloads, Back cancels. The size comes from the search API's `download_size`, so the prompt appears before any bandwidth is spent.

**Files changed**: `BookFusionSyncClient.{cpp,h}`, `BookFusionBrowserActivity.{cpp,h}`, `english.yaml` (`STR_BF_LARGE_BOOK_WARNING`)

### BookFusion reading position carries into the library on download
On download, the synced BookFusion position now stores the book-level percentage straight from the API (`remotePos.percentage`, the authoritative value) into `RecentBooksStore`, instead of recomputing from `chapterIndex` — which defaults to 0 when the API omits `chapter_index` and showed 0% for partway-read books. The library/home progress reads from `RecentBooksStore`, so the synced percentage now appears immediately without opening the book.

**Files changed**: `BookFusionBrowserActivity.cpp`

### BookFusion covers always come from the API image, never the EPUB
BookFusion-served EPUBs frequently carry broken or missing embedded covers, so cover handling for BookFusion books no longer falls back to EPUB extraction — the only source is the already-normalised API image cached at download (`thumb_<H>.bmp` + full `cover.bmp`). Two follow-on fixes: (1) the download thumbnail is now generated grayscale (`jpeg/pngFileToBmpStreamWithSize`) instead of 1-bit — the 1-bit converter failed on some cover JPEGs, leaving `thumb_<H>.bmp` missing while the grayscale `cover.bmp` succeeded; (2) `LibraryActivity` falls back to the full `cover.bmp` when the tile thumbnail is absent, and never runs `generateThumbBmp` for a BookFusion-linked book (`hasBookId`). Also fixed a render-loop wedge where a slot whose cover couldn't be generated was re-selected every frame (endless "Loading…" flashing, no covers), via a per-page `coverGenAttempted` guard.

**Files changed**: `BookFusionBrowserActivity.cpp`, `LibraryActivity.{cpp,h}`

### Library auto-refreshes after books are added/removed
The library index (`/.crosspoint/library_index.bin`) validates its cache by directory mtime, but adding a file to the FAT root doesn't bump the root's mtime — so newly downloaded/uploaded books didn't appear (the file browser, which scans live, did show them). `LibraryActivity::invalidateIndexCache()` is now called from every add/remove path: BookFusion download, OPDS download, web upload, and WebDAV PUT/DELETE. A **Recache Library** action (System tab, Developer Mode only) forces a rescan for any remaining edge case. Added a `const String&` overload for `FsHelpers::hasXtcExtension` to match the other extension helpers.

**Files changed**: `LibraryActivity.{cpp,h}`, `BookFusionBrowserActivity.cpp`, `OpdsBookBrowserActivity.cpp`, `CrossPointWebServer.cpp`, `WebDAVHandler.cpp`, `SettingsActivity.{cpp,h}`, `FsHelpers.h`, `english.yaml` (`STR_RECACHE_LIBRARY`, `STR_LIBRARY_RECACHED`)

### Sleep screen: power off the charge pump on the final refresh
The e-ink charge pump was left energized when entering deep sleep, showing as noise/ghosting on the sleep image. The terminating sleep refresh now passes `powerOffAfter=true` to `displayBuffer`, collapsing the panel rails (the greyscale bitmap path powers down on its own, so it's only set on the final paint).

**Files changed**: `SleepActivity.cpp`

### Settings & stats tidy-ups
- Reading-stats settings (daily reading goal, minimum session length) regrouped under the Stats category in `SettingsList`, so they sit together on the device and on the web settings page.
- Reading Stats button hints now show the next tab's name on Confirm (ribbon focus), matching `SettingsActivity`'s convention.

**Files changed**: `SettingsList.h`, `ReadingStatsActivity.cpp`

### Fixes
- Lyra power-button hint box now fills white first so list/book content no longer bleeds through it.

**Files changed**: `LyraTheme.cpp`

---

## 1.7.0 — June 2026

### Configurable Reader Controls + on-screen button hints
The reader's buttons are now assignable. **Settings → Reader Controls** binds a **short-press** and **long-press** action to each button, with a live preview and Confirm-to-cycle / Back-to-save flow. Available actions include Chapter Forward/Back, Menu, Files, Sync, Bookmark, Screenshot, Mark Finished, Auto Turn, Bionic, Button Hints, and Rotate. A new **Button Hints** option draws small labels next to each physical button while reading; hint placement modes are **Fixed**, **Front Only Short**, and **Front Only Long**. Bindings attach to logical button roles, so they follow any front-button remap.

**Files changed**: `ReaderControlsActivity.{cpp,h}`, `EpubReaderActivity.{cpp,h}`, `TxtReaderActivity.cpp`, `XtcReaderActivity.cpp`, `ReaderUtils.h`, `CrossPointSettings.h`, `SettingsList.h`, `english.yaml`

### Bionic Reading mode
New **Bionic Reading** toggle that bolds the leading portion of each word to guide the eye, available as a reader setting and an assignable button action. Re-lays out text, so cached sections regenerate on first open.

**Files changed**: `ParsedText.{cpp,h}`, `Section.cpp`, `GfxRenderer.{cpp,h}`, `EpubReaderActivity.cpp`, `CrossPointSettings.h`

### Book Info panel
A new **Book Info** screen surfaces full metadata for the open book: series & number, bookshelf, categories, lists, publisher, published date, tags, rating, language, and the full description (with a "No description available" fallback).

**Files changed**: `EpubReaderMenuActivity.{cpp,h}`, `english.yaml` (`STR_BOOK_INFO_*`, `STR_DESCRIPTION`, `STR_NO_DESCRIPTION`)

### More BookFusion metadata & sync
Sync now records and updates **reading time** alongside position, and pulls richer metadata (series, bookshelf, tags, rating, lists, description) that feeds the Book Info panel. New **BookFusion sort** modes — *BookFusion first / last* and *Tag A–Z / Z–A* — across the library and file lists.

**Files changed**: `BookFusionSyncActivity.{cpp,h}`, `BookFusionBrowserActivity.cpp`, `SortMode.{cpp,h}`, `english.yaml` (`STR_SORT_BOOKFUSION_*`, `STR_SORT_TAG_*`)

### Better X3 support
Button hints are X3-aware: the Power-button hint tracks the X3's physical top-right Power button, and side-button hint boxes sit against the correct physical edges with proper margin reservations across all reading orientations. Page text is padded so hint boxes never overlap content.

**Files changed**: `BaseTheme.{cpp,h}`, `LyraTheme.{cpp,h}`, `LyraLibraryTheme.cpp`

### Upstream fixes & cleanup
Rendering/layout fixes in `GfxRenderer`, `ParsedText`, `Section`, and footnote handling, plus a settings/i18n cleanup that removed a batch of dead string keys and shortened navigation labels for the hint UI.

---

## Prior releases (1.6.x and earlier)

The sections below document earlier work on top of the `master` branch.

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
