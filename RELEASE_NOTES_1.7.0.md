# CrossPoint Reader 1.7.0

A feature-focused release: a fully configurable reader button system with on-screen hints, a book info panel, Bionic Reading, expanded BookFusion metadata/sync, better X3 layout, and a batch of rendering fixes ported from upstream.

---

## 🎛️ Configurable reader buttons + on-screen button hints

The reader's buttons are now assignable. **Settings → Reader Controls** lets you map short-press and long-press actions per button, with a live preview:

- **Short press / Long press** action slots, cycled with Confirm and saved with Back.
- Hint modes: **Fixed**, **Front Only Short**, **Front Only Long**.
- Assignable actions: Chapter Forward/Back, Menu, Files, Sync, Bookmark, Screenshot, Mark Finished, Auto Turn, Bionic, Button Hints, Rotate.
- **Button Hints** (Settings) draws small on-screen labels next to each physical button while reading, so you always know what each press does.

## 📖 Bionic Reading mode

New **Bionic Reading** toggle that bolds the leading portion of each word to help guide the eye. Available as a reader setting and as an assignable button action.

> Note: enabling/disabling Bionic re-lays out text, so cached sections regenerate on first open.

## ℹ️ Book Info panel

A new **Book Info** screen surfaces full metadata for the open book: Series & Number, Bookshelf, Categories, Lists, Publisher, Published date, Tags, Rating, Language, and a full **Description** (with a graceful "No description available" fallback).

## ☁️ More BookFusion metadata & sync

- Sync now records and updates **reading time** alongside position.
- Richer BookFusion metadata pulled into the device (feeding the new Book Info panel).
- New **BookFusion sort** modes in library/file lists: *BookFusion first / last*, plus *Tag A–Z / Z–A*.

## 📱 Better X3 support

Button hints are now X3-aware: the Power-button hint tracks the X3's physical top-right Power button, and side-button hint boxes are placed against the correct physical edges with proper margin reservations across all reading orientations (the X4's edge-mounted layout is handled separately). Page text is padded so hint boxes never overlap content.

## 🐛 Upstream fixes & polish

- Rendering/layout fixes ported from upstream (`GfxRenderer`, `ParsedText`, `Section`, footnote layout).
- Settings cleanup: removed dead/unused options and tidied i18n strings.
- Shorter, hint-friendly navigation labels.

---

### Install

Flash `firmware.bin` via OTA (**Settings → Check for updates**), Download-from-URL, or SD-card recovery. An SD card must be present for OTA (the image is staged there before the raw write).

**Cache:** if you previously read books on 1.6.x, layout-affecting changes (Bionic, footnotes) may require clearing `.crosspoint/` on the SD card for already-opened books to re-render correctly.
