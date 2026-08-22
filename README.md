# KatiePoint Reader

A heavily-customised personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — open-source firmware for **Xteink** e-paper devices (unaffiliated with Xteink). Built using **PlatformIO**.

| Device | MCU | Status |
| ------ | --- | ------ |
| **Xteink X4** | ESP32-C3 | Primary target |
| **Xteink X3** | ESP32-C3 | Supported — same binary, board profile picked at boot |
| **Xteink X4 Pro** | ESP32-S3 | **Experimental**, in bring-up — separate binary (`pio run -e x4pro`), touch + frontlight |

![](./docs/images/logo.png)

## What's different in this fork

Everything below has been added or significantly reworked on top of the upstream CrossPoint codebase. See [CHANGES.md](./CHANGES.md) for the full technical writeup.

- **Library cover grid** — paginated 3×N grid of every book on the SD card, via a new **Lyra Library** UI theme. Cover thumbnails cached to SD; same Book Options menu (Book Info, Mark as Read, Remove from Recents, Delete from Device — plus Reset Progress / Delete Book Cache / Regenerate Cover in Developer Mode) as the home recents tile.
- **BookFusion sync** — OAuth device-code account linking, browse your cloud library by category (Currently Reading / Favorites / Plan to Read / Completed / All Books), download EPUBs directly to the SD card, and sync reading progress *and reading time* bidirectionally via long-press Confirm. Pulls rich metadata (series, bookshelf, tags, rating, lists, description) that feeds the Book Info panel, and adds *BookFusion first/last* and *Tag A–Z/Z–A* sort modes. A `& ` prefix marker shows synced books across all list views.
- **Bionic Reading** — optional mode that bolds the leading portion of each word to guide the eye. Toggle in reader settings or bind it to a button.
- **Book Info panel** — full metadata for the open book (series & number, bookshelf, categories, lists, publisher, published date, tags, rating, language) plus the full description.
- **Sortable lists** — 14 sort modes (Name A-Z/Z-A, Author A-Z/Z-A, Recently/Least recently opened, Most/Least read, Newest/Oldest on device, Date added, BookFusion first/last, Tag A–Z/Z–A) across Library, Recent Books, and File Browser. Short-press Power opens the Sort Menu; the chosen mode persists across reboots.
- **Folder views** — Browse Files can group the SD card by real **Folders** (default), or by **Tags**, **Authors**, or **Series** (Settings → Display → Folder View). Grouping pulls from BookFusion/EPUB metadata; open a group to see its books.
- **Reading Stats overhaul** — 7 metrics instead of 3 (Reading Time, Pages Read, Books Finished, Sessions, Avg. Session, Reading Speed, In Progress), now split across **Overview / Books / Week / Month / Sessions** tabs with reading streaks, a daily goal, a reading-profile score, and a Month calendar heat-map. Adds **Export / Import** and **Export to StoryGraph**, and can surface up to three stats on the sleep screen. Accessible from the Home menu; session tracking covers `.epub`, `.txt`, and `.xtc` instead of EPUB only.
- **Book Options popup** — long-press Confirm on a recent book opens an inline modal (Mark as Read, Shelve, Reset Progress, etc.) instead of navigating away. Same modal in the Library cover grid.
- **Mark as Completed** — in-reader menu option that bumps progress to 100%, increments Books Finished, and returns home. Books at ≥90% are now counted as finished (was 100%).
- **Inline footnotes** — new "On page" Footnotes mode renders footnote text at the bottom of the page that references it, beneath a horizontal rule. Space is reserved during layout so the footnote block never overlaps page text.
- **Bookmarks & Clippings** — bookmark any page and jump back to it, or save text **clippings** (highlights) from a page and review them per book. Both are in the reader menu, and Bookmark can be bound to a button or short-Power press.
- **OPDS Browser** — point KatiePoint at an OPDS catalogue (e.g. Calibre Content Server) and browse/download books over WiFi. Appears on the Home menu once a server URL is configured.
- **Customise Status Bar** — every element is positioned independently: battery, book title, chapter title, book %, chapter page count, book/chapter time-left, clock, a **bookmark indicator** (shown only on bookmarked pages) and a **Bluetooth indicator** each cycle **Hide → Left → Center → Right**. Plus a Book/Chapter progress bar (with thickness) and an adjustable **top margin** between body text and the bar. A **Hide Status Bar** reader action toggles the whole bar off and re-centres the page without reflowing it (the layout cache stays valid). Old show/hide settings migrate automatically on first boot. Also editable from the web settings page.
- **Auto-Turn in seconds-per-page** — replaces the older pages-per-minute setting. Options: Off / Auto / 60s / 50s / 40s / 35s / 30s / 25s / 20s. Auto uses your calibrated reading speed; the menu shows the estimate inline, e.g. `Auto (~45s)`.
- **Typography** — new X-Small font size across Bookerly / Inter / OpenDyslexic; new **Monospace** family (JetBrains Mono 6/8/10/12 pt) for code-heavy EPUBs. Bold-italic and 18 pt variants removed to reclaim flash.
- **Configurable Reader Controls + button hints** — bind a short-press and long-press action to each reader button (Settings → Reader Controls), choosing from chapter skip, menu, files, sync, bookmark, screenshot, mark finished, auto-turn, bionic, rotate, and more. Optional **on-screen button hints** draw a small label next to each physical button while reading so you always know what each press does, with hint modes (Fixed / Front Only Short / Front Only Long). Bindings attach to *logical* button roles, so they follow any front-button remap.
- **Better X3 support** — button hints are X3-aware: the Power-button hint tracks the X3's physical top-right Power button and side-button hints sit against the correct physical edges, with page text padded so hint boxes never overlap content in any orientation.
- **Bluetooth page turner** — pair a cheap BLE remote/clicker and turn pages without touching the device. A guided setup walks you through enable → scan → pair → **map buttons** → live press test. Mapping is optional: leave it and *any* button pages forward (what makes the wildly inconsistent cheap clickers work at all), or teach it two buttons and get **page back** on the remote as well. The firmware detects when a learned mapping has gone stale and prompts you to re-map. Bluetooth still shuts itself down whenever the RAM is needed elsewhere (syncing, chapter indexing, outside the reader), but it now **restores itself automatically** once the reader is idle again — including after an autosync push, which no longer disables the remote for the session. An optional status-bar indicator shows Off / On / Connected. See the [User Guide](USER_GUIDE.md#11-bluetooth-page-turner).
- **Full Touch mode** *(X4 Pro)* — opt-in tap-to-hit-test UI (**Settings → System → Full Touch Mode**): tap a row to select it, tap again to activate. Vertical swipes page whole lists, a rightward swipe steps tabs, a leftward swipe is Back. Off by default, where taps just inject Confirm on the current selection. Reader **tap zones** (left/middle/right thirds), **hold zones**, and the capacitive **home key** (short/long) are all bindable in Reader Controls.
- **Frontlight** *(X4 Pro)* — brightness 0–100% and, on the warm/cool pair, a warmth mix. Adjustable from **Settings → Display** or in-place from the reader menu, so you never leave the page to change the light.
- **Reader menu revamp** — a two-line summary at the top (reading speed, chapter position + time left, book % + time left / clock, date, today's reading vs your daily goal), in-place cycling rows for Dark Mode, Button Hints, Orientation and Frontlight, and per-provider sync rows (**Sync: Push to BookFusion**, **Pull from KOReader**, …) that only appear when that book actually has a sync backend. The Reading Speed and Mark-as-Read rows are gone — speed now lives in the summary line. Individual sections (Clippings / Bookmarks / Bluetooth / Sync) can be hidden from **Settings → Reader** if you drive them from buttons instead.
- **Progress Autosync** — silent background progress push (Off / Every Chapter / Every 5% / On Exit). Coexists with a Bluetooth remote: the BLE stack is torn down for the WiFi session and brought back afterwards.
- **Hyphenation toggle** — dictionary hyphenation is back as a Reader setting rather than a build-time constant.
- **File Browser progress column** — right-aligned `X` (finished, ≥90%), `21%` (in progress), or blank (unopened/directory) on every row.
- **Sleep screen options** — Dark / Light / Custom / **Cover** / **Cover + Custom** / None, with cover Fit/Crop and None/Contrast/Inverted filters. Drop multiple `.bmp`s in a `.sleep/` folder for a random image each sleep, and overlay up to three reading stats.

## Original CrossPoint features (still here)

- EPUB 2 / EPUB 3 parsing and rendering with image support
- Saved reading position, custom sleep screen (incl. book-cover mode)
- WiFi book upload, WiFi OTA updates
- KOReader Sync for cross-device reading progress
- Configurable font, layout, and display options
- Screen rotation (all 4 orientations)
- File explorer with nested folders
- Multi-language UI — English, Spanish, French, German, Italian, Portuguese, Russian, Ukrainian, Polish, Swedish, Norwegian, [and more](./USER_GUIDE.md#supported-languages)

See [the user guide](./USER_GUIDE.md) for operating instructions, including the [KOReader Sync quick setup](./USER_GUIDE.md#koreader-sync-quick-setup).

For project scope, see [SCOPE.md](SCOPE.md).

## Installing

Prebuilt firmware is published on the [Releases page](https://github.com/InsiderPhD/crosspoint-reader/releases). Already running KatiePoint? Update over the air via **Settings → Check for updates** (OTA pulls the latest release from this repo; an SD card must be inserted while it stages the image). You can also flash a release `firmware.bin` via Download-from-URL or SD-card recovery. To build from source instead, see [Development](#development) below.

If you want the official **upstream CrossPoint** firmware instead — flashable from a web page with no local toolchain — head to https://xteink.dve.al/. That site can also revert your device to the original Xteink firmware via the "Swap boot partition" controls at https://xteink.dve.al/debug.

## Development

### Prerequisites

* **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
* Python 3.8+
* USB-C cable for flashing the device
* An Xteink X4, X3, or X4 Pro

### Checking out the code

```
git clone --recursive <your repo URL>

# Or, if you've already cloned without --recursive:
git submodule update --init --recursive
```

### Flashing your device

Connect your device to your computer via USB-C and run:

```sh
# X4 / X3 (ESP32-C3) — the default environment
pio run --target upload

# X4 Pro (ESP32-S3) — a separate binary, not an addition to the C3 image
pio run -e x4pro --target upload
```

The default (`pio run`) build produces a version string like `Dev-KT-v1.2.0-dev+<branch>`, visible on the boot screen and in Settings.

> [!NOTE]
> **One binary per MCU family.** X3 and X4 ship in the same C3 image and the board
> profile is resolved at boot from an I²C fingerprint. The X4 Pro is an ESP32-S3 with
> PSRAM, a GT911 digitizer, a capacitive home pad, a PWM frontlight and native SDMMC —
> `freeink-sdk` hard-errors if devices from two MCU families are selected at once, so
> `[env:x4pro]` unsets the C3 device flags rather than adding to them. The X4 Pro port
> is still in bring-up; expect rough edges.

### Debugging

After flashing, capture serial logs:

```python
python3 -m pip install pyserial colorama matplotlib
```

```sh
# For Linux
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

## Internals

KatiePoint is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM and no PSRAM, so we have to be careful — a lot of the firmware's design decisions are downstream of that constraint. (The X4 Pro's S3 has PSRAM, but the code is shared, so it inherits the same discipline.)

### Data caching

The first time chapters of a book are loaded, they're cached to the SD card. Subsequent loads come from the cache. The cache directory is `.crosspoint` on the SD card (the on-disk name is inherited from upstream CrossPoint and unchanged in this fork to preserve reading progress on existing installs). Structure:

```
.crosspoint/
├── epub_12471232/       # Each EPUB is cached to a subdirectory named `epub_<hash>`
│   ├── progress.bin     # Stores reading progress (chapter, page, etc.)
│   ├── cover.bmp        # Book cover image (once generated)
│   ├── book.bin         # Book metadata (title, author, spine, table of contents, etc.)
│   └── sections/        # All chapter data is stored in the sections subdirectory
│       ├── 0.bin        # Chapter data (screen count, all text layout info, etc.)
│       ├── 1.bin        #     files are named by their index in the spine
│       └── ...
│
└── epub_189013891/
```

Deleting `.crosspoint` clears the entire cache. Moving or renaming a book file changes its hash and resets its reading progress.

For more on the internal file structures, see [the file formats document](./docs/file-formats.md).

## Contributing

This is a personal fork — I'm not actively soliciting external contributions here. If you want to contribute to the broader project, the upstream [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) is the better venue. For more on community principles (inherited from upstream), see [GOVERNANCE.md](GOVERNANCE.md).

---

KatiePoint Reader is **not affiliated with Xteink or any manufacturer of the X4 hardware**.

Built on top of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), which itself took a lot of inspiration from [**diy-esp32-epub-reader** by atomic14](https://github.com/atomic14/diy-esp32-epub-reader). Huge thanks to both.
