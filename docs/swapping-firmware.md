# Switching between KatiePoint, CrossPoint and CrossInk

KatiePoint, upstream CrossPoint (2.0) and CrossInk run on the same Xteink boards and share one SD card
layout, so you can reflash between them. This page lists what carries over and what does not.

## How to switch

Each firmware's over-the-air updater only offers its own releases. To switch, install the other
firmware's `.bin` from the SD card (**SD Card Firmware Update**, or hold the recovery buttons at power-on: Up+Power on X3/X4, Down+Power on X4 Pro/X4C), from a URL, or over USB.
All three refuse an image built for a different board (`CROSSPOINT-BOARD-V1` tag) or chip.

## What carries over

| Data | KatiePoint ↔ CrossPoint 2.0 | KatiePoint ↔ CrossInk |
|---|---|---|
| Flash layout / partitions | identical | identical |
| Wi-Fi networks (`wifi.json`) | yes | yes |
| KOReader / CrossPoint Sync account (`koreader.json`) | yes, incl. server type and custom headers | yes |
| OPDS catalogs (`opds.json`) | yes | yes |
| Shared settings (font size, margins, orientation, buttons, clock, …) | yes, translated both ways | imported once by CrossInk |
| KatiePoint-only settings (status bar layout, reader actions, sleep theme, frontlight schedule, …) | kept in `katiepoint-settings.json`, untouched by the other firmware | same |
| UI language | yes | yes (via `language.bin` / `.bak`) |
| Reading position (`progress.bin`) | yes | yes — KatiePoint renames CrossInk's cache folder back on open |
| SD fonts (`.cpfont`), dictionaries, sleep images, plugins | yes | yes |
| KOReader document hash | same | same |

## What does not carry over

- **Page caches** (`book.bin`, `sections/`, CSS): each firmware tags its own and rebuilds the others'.
  Expect one re-index per book after switching.
- **Clippings**: KatiePoint keeps its own (`kp_*.bin`). Upstream clippings are shown read-only;
  CrossInk's are ignored.
- **Bookmarks** between KatiePoint and CrossInk (different formats). KatiePoint ↔ CrossPoint is fine.
- **Reading statistics** (each firmware has its own format).
- **Bluetooth remotes**: pair again after switching.
- **Library index**: each firmware builds its own.
- Settings the other firmware cannot represent (e.g. CrossPoint's Cover Grid theme) keep their own value
  on each side.

## Settings files in detail

- KatiePoint reads and writes `/.crosspoint/katiepoint-settings.json`.
- On first boot without that file it imports `/.crosspoint/settings.json` (upstream or an older
  KatiePoint) once.
- After each settings save it updates the shared keys in `settings.json` in upstream's numbering and
  leaves upstream's other keys alone (skipped while memory is low; the next save catches up).
- Changes CrossPoint makes to `settings.json` are not pulled back once `katiepoint-settings.json`
  exists. Delete `katiepoint-settings.json` to re-import.

See `docs/file-formats.md` for the exact formats and the cache fork markers.
