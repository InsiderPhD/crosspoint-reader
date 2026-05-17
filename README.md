# KatiePoint Reader

A heavily-customised personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — open-source firmware for the **Xteink X4** e-paper device (unaffiliated with Xteink). Built using **PlatformIO** and targeting the **ESP32-C3**.

![](./docs/images/logo.png)

## What's different in this fork

Everything below has been added or significantly reworked on top of the upstream CrossPoint codebase. See [CHANGES.md](./CHANGES.md) for the full technical writeup.

- **Library cover grid** — paginated 3×N grid of every book on the SD card, via a new **Lyra Library** UI theme. Cover thumbnails cached to SD; same six-option book menu (mark read, reset progress, shelve, delete, reindex, regenerate cover) as the home recents tile.
- **BookFusion sync** — OAuth device-code account linking, browse your cloud library by category (Currently Reading / Favorites / Plan to Read / Completed / All Books), download EPUBs directly to the SD card, and sync reading progress bidirectionally via long-press Confirm. A `& ` prefix marker shows synced books across all list views.
- **Sortable lists** — 10 sort modes (Name A-Z/Z-A, Author A-Z/Z-A, Recently/Least recently opened, Most/Least read, Newest/Oldest on device) across Library, Recent Books, and File Browser. Short-press Power opens the Sort Menu; the chosen mode persists across reboots.
- **Reading Stats expansion** — 7 metrics instead of 3 (Reading Time, Pages Read, Books Finished, Sessions, Avg. Session, Reading Speed, In Progress). Now accessible from the Home menu, and session tracking covers `.epub`, `.txt`, and `.xtc` instead of EPUB only.
- **Book Options popup** — long-press Confirm on a recent book opens an inline modal (Mark as Read, Shelve, Reset Progress, etc.) instead of navigating away. Same modal in the Library cover grid.
- **Mark as Completed** — in-reader menu option that bumps progress to 100%, increments Books Finished, and returns home. Books at ≥90% are now counted as finished (was 100%).
- **Inline footnotes** — new "On page" Footnotes mode renders footnote text at the bottom of the page that references it, beneath a horizontal rule. Space is reserved during layout so the footnote block never overlaps page text.
- **Auto-Turn in seconds-per-page** — replaces the older pages-per-minute setting. Options: Off / Auto / 60s / 30s / 10s / 5s. Auto uses your calibrated reading speed; the menu shows the estimate inline, e.g. `Auto (~45s)`.
- **Typography** — new X-Small font size across Bookerly / NotoSans / OpenDyslexic; new **Monospace** family (JetBrains Mono 6/8/10/12 pt) for code-heavy EPUBs. Bold-italic and 18 pt variants removed to reclaim flash.
- **Global long-press Back → Home** — holding Back for 1 second returns to Home from anywhere in the firmware.
- **File Browser progress column** — right-aligned `X` (finished, ≥90%), `21%` (in progress), or blank (unopened/directory) on every row.

## Original CrossPoint features (still here)

- EPUB 2 / EPUB 3 parsing and rendering with image support
- Saved reading position, custom sleep screen (incl. book-cover mode)
- WiFi book upload, WiFi OTA updates
- KOReader Sync for cross-device reading progress
- Configurable font, layout, and display options
- Screen rotation (all 4 orientations)
- File explorer with nested folders
- Multi-language UI — English, Spanish, French, German, Italian, Portuguese, Russian, Ukrainian, Polish, Swedish, Norwegian, [and more](./USER_GUIDE.md#supported-languages)

See [the user guide](./USER_GUIDE.md) for operating instructions, including the [KOReader Sync quick setup](./USER_GUIDE.md#365-koreader-sync-quick-setup).

For project scope, see [SCOPE.md](SCOPE.md).

## Installing

This fork is personal-use and doesn't publish release binaries. To install KatiePoint, build from source (see [Development](#development) below).

If you want the official **upstream CrossPoint** firmware instead — flashable from a web page with no local toolchain — head to https://xteink.dve.al/. That site can also revert your device to the original Xteink firmware via the "Swap boot partition" controls at https://xteink.dve.al/debug.

## Development

### Prerequisites

* **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
* Python 3.8+
* USB-C cable for flashing the ESP32-C3
* Xteink X4

### Checking out the code

```
git clone --recursive <your repo URL>

# Or, if you've already cloned without --recursive:
git submodule update --init --recursive
```

### Flashing your device

Connect your Xteink X4 to your computer via USB-C and run:

```sh
pio run --target upload
```

The default (`pio run`) build produces a version string like `Dev-KT-v1.2.0-dev+<branch>`, visible on the boot screen and in Settings.

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

KatiePoint is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful — a lot of the firmware's design decisions are downstream of that constraint.

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
