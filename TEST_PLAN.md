# Hardware Test Plan — X3 / X4 / X4 Pro

Per-device regression + verification checklist for the current working tree.
Each device section is **self-contained** — run it top to bottom with that device
on the desk and cross boxes off as you go.

## Global preconditions (read before flashing anything)

- [ x ] Build from the **current working tree, including the `freeink-sdk` submodule diff**
      (the reimplemented `FreeInkDisplay::powerOffIdle()` lives uncommitted in the
      submodule — a clean submodule checkout silently reverts the X4 Pro display fixes).
- [ x ] `pio run` completes with 0 errors/warnings. Several pending changes have never
      been built (autosync, BLE reconnect fixes, X3 TLS fix, speckle repaint) — fix
      compile errors before any hardware time.
- [ ] For the X3 TLS test: BookFusion/OPDS/OTA now go through wolfSSL (`SecureNet`),
      not the old asymmetric-mbedTLS sdkconfig — confirm `FREEINK_NET_WOLFSSL=1` and
      `WOLFSSL_SP_RISCV32` are in the built image before flashing, or the test is
      meaningless. KOReader sync is still on esp-tls and is a separate check.
- [ ] Record per session: date, `git rev-parse --short HEAD`, submodule dirty (y/n),
      environment (`default` / `gh_release_rc`).
- [ ] Delete `.crosspoint/` on the SD card before the "fresh cache" tests below
      (render-affecting code changed; stale sections will mask layout bugs).
- [ ] Test SD card has: ≥1 large EPUB (5MB+, with images), 1 small EPUB, 1 TXT,
      1 XTC, a book with footnotes, and books with tags/authors metadata.

Session log:

| Date | Device | Commit | Submodule dirty? | Env | Notes |
|------|--------|--------|------------------|-----|-------|
|      |        |        |                  |     |       |

---

# X4 (baseline device)

## Preconditions
- [ ] Flashed current-tree build; serial monitor attached (`python3 scripts/debugging_monitor.py`).

## Boot & smoke
- [ ] Cold boot reaches Home with no crash screen; SD card detected, library scans.
- [ ] Free heap after boot > 50KB (serial `ESP.getFreeHeap()`), and again after opening/closing a book.
- [ ] **SDK migration display parity**: full refresh, partial refresh, and grayscale
      cover images all look identical to pre-migration firmware (no banding, no
      inverted regions, no offset image).

## Core reading (fresh cache — `.crosspoint/` deleted)
- [ ] Large EPUB opens: parse + section build completes, first page renders, no
      reboot, heap stays > 50KB during build.
- [ ] 20+ page turns forward and back with side buttons — correct order, no artifacts.
- [ ] Chapter select opens and jumps correctly; footnotes open and return.
- [ ] Bookmarks: add, list, jump, delete.
- [ ] Close book, reopen — progress restored to the same page.
- [ ] Progress write throttling: turning pages rapidly does not log a settings write
      per turn (debounced / on-exit only).
- [ ] TXT reader and XTC reader both open and page correctly.
- [ ] Bionic reading toggle on → text re-renders bolded prefixes; toggle off restores.
- [ ] Grayscale images in EPUB render (single-buffer store/restore path — no
      leftover corruption on the page after an image).

## Clippings (OOM fix retest)
- [ ] Start clip selection on a **long, dense chapter** (the old OOM-abort repro):
      selection UI opens, word-by-word selection works, no reboot.
- [ ] Save a clipping; it appears in the clipping list; delete it.
- [ ] Heap before/after clip selection roughly equal (no leak from the pooled WordRef path).

## UI changes (landed in `86c56619`)
- [ ] **Status bar per-element positions**: for each element, cycle Hide / Left /
      Middle / Right in settings — layout updates live, nothing overlaps, bookmark
      icon appears on bookmarked pages.
- [ ] **Reader menu revamp**: two-line summary shows speed/chapter/book progress +
      clock/date/goal; "Reading Speed" and "Mark as Read" rows are **gone**;
      visibility toggles hide/show their rows correctly.
- [ ] Tags and Authors folder views (Group browser) list correctly; entering a
      group shows its books; dev recache works.
- [ ] Font & Layout preview: live split-screen updates on font/size/spacing change;
      exiting restores the correct orientation.

## Orientations (all 4: Portrait / Inverted / Landscape CW / CCW)
- [ ] Home, Library, reader, and reader menu render correctly in all 4 modes
      (no clipped text, status bar in the right place).
- [ ] Page-forward/page-back follow `sideButtonLayout` (and its swap setting) in all 4.
- [ ] Front-button remapping still honored after orientation change.
- [ ] Power-button UI hints render on the **right edge, above Up/Down** (X4 position).

## Network & sync
- [ ] WiFi connects; web server reachable; upload a book via browser — appears in library.
- [ ] OPDS browse + download works.
- [ ] BookFusion sync completes (wolfSSL path) — books download, no TLS error.
- [ ] KOReader progress sync completes (still on the pre-wolfSSL esp-tls path — regression check).
- [ ] **Ghosting on sync timeout**: interrupt WiFi mid-sync — screen recovers with a
      cleanup refresh, no ghost overlay left behind.
- [ ] **Autosync**: with autosync on, finish a reading session — progress pushes
      silently in the background (verify via serial/server), reader UI never blocks.
- [ ] **Autosync ⟷ Bluetooth mutual exclusion, both directions**: enabling BT
      disables autosync; enabling autosync disables BT; no state where both run.
- [ ] **Cold-boot browse crash**: cold boot with WiFi configured, immediately enter
      Library/Tags while recache is running — no crash (NTP-preempt candidate fix;
      repeat 3× since the original crash was intermittent). Note any `MEMDIAG` spam
      — those logs are due for removal.
- [ ] OTA update check reaches the InsiderPhD repo and reports a version.

## Power & sleep
- [ ] Auto-sleep fires after the timeout; short power press wakes from deep sleep.
- [ ] **Battery-only sleep ghosting** (USB **unplugged** — VBUS masks the bug):
      read 20 pages, let it sleep — sleep screen paints via FULL_REFRESH with no
      ghost of the last page; wake and check again.
- [ ] Battery percentage plausible and stable across a sleep/wake cycle.

## Bluetooth (BLE page turner)
- [ ] Pairing wizard finds and pairs AB Shutter3; page turns work in reader.
- [ ] **Two-button signature wizard**: map a two-button remote — both buttons
      detected as distinct signatures, forward/back assigned correctly.
- [ ] Remote reconnects after device sleep/wake and after the remote's address
      rotates (re-pair flow works if it doesn't).
- [ ] **Auto-sleep still fires with a paired remote idle nearby** (activity is
      keyed on real presses now — an advertising remote must not hold the device awake).
- [ ] BLE is torn down outside reader/BT settings (serial: heap returns).
- [ ] Chapter section build while BLE active succeeds (spine/TOC lease headroom —
      no 32KB-contiguous inflate failure).
- [ ] **BLE auto-restore**: with BT wanted, trigger a chapter build or sync —
      BLE comes back ~3s after WiFi settles, **no freeze** (this is the flagged
      freeze-risk path; watch serial closely).
- [ ] 80MHz CPU floor holds with BLE on — no silent freeze during idle reading.

## Watch-for (note anything seen, even if tests pass)
- Heap after repeated open/close of large books (fragmentation creep).
- WDT: if the UI ever wedges >5 min it should panic with a trace, reboot, and
  auto-sleep — and the crash screen should count it. Note if a wedge instead hangs forever.

---

# X3

## Preconditions
- [ ] Flashed current-tree build (incl. submodule diff); serial attached.
- [ ] Confirm boot log selects the **X3 display profile** (`selectDevice(X3)` early).

## Boot & smoke
- [ ] Cold boot to Home, SD detected, library scans, heap > 50KB.
- [ ] **SDK migration display parity on the X3 panel specifically** (this is the
      device where parity is unverified): full refresh, partial refresh, grayscale
      covers — compare against pre-migration firmware if available.

## X3-specific
- [ ] **Status-bar clock** driven by DS3231: correct time immediately on boot
      (no WiFi), ticks over the minute.
- [ ] **Date via NTP-on-boot**: after a WiFi connect, reading-stats date is correct;
      after a cold boot *without* WiFi, behavior matches design (date from last NTP,
      not from RTC).
- [ ] **BookFusion manual sync — TLS OOM fix**: run a manual sync from a fragmented
      state (open/close a large book first, then sync). Must complete with **no "-1"
      error**. Check serial for the wolfSSL TLS 1.3 handshake succeeding; repeat 3×.
- [ ] Power-button UI hints render **top-right** (X3 position), not on the right edge.

## Core reading (fresh cache)
- [ ] Large EPUB: build + render, 20+ page turns both directions, no artifacts.
- [ ] Chapter select, footnotes, bookmarks (add/jump/delete).
- [ ] Progress restored on reopen; writes debounced.
- [ ] TXT and XTC readers work.
- [ ] Bionic reading toggle works.
- [ ] Grayscale EPUB images render cleanly.

## Clippings
- [ ] Clip selection on a long chapter — no OOM reboot; save, list, delete a clipping.

## UI changes
- [ ] Status bar per-element Hide/L/M/R + bookmark icon.
- [ ] Reader menu revamp: two-line summary, removed rows gone, visibility toggles work.
- [ ] Tags/Authors folder browse; Font & Layout preview.

## Orientations (all 4)
- [ ] Home, reader, menus correct in all 4 modes on the X3 panel geometry.
- [ ] Page buttons follow side-button layout setting in all 4.

## Network & sync
- [ ] WiFi + web server upload; OPDS download.
- [ ] KOReader progress sync completes (pre-wolfSSL esp-tls path — regression check).
- [ ] Sync-timeout ghosting: interrupt WiFi mid-sync — cleanup refresh, no ghost.
- [ ] Autosync silent push works; autosync ⟷ BT mutual exclusion both directions.
- [ ] Cold boot with WiFi → straight into Library/Tags during recache — no crash (3×).
- [ ] OTA check works.

## Power & sleep
- [ ] Auto-sleep fires; short power press wakes from deep sleep.
- [ ] **Battery-only** sleep paint: FULL_REFRESH, no ghosting (USB unplugged).
- [ ] Battery percentage plausible.

## Bluetooth
- [ ] Pair AB Shutter3; page turns in reader; reconnect after sleep/wake.
- [ ] Two-button signature wizard maps both buttons.
- [ ] Auto-sleep still fires with idle paired remote nearby.
- [ ] BLE torn down outside reader/settings; chapter build succeeds with BLE on.
- [ ] BLE auto-restore after build/sync — no freeze.

## Watch-for
- X3 has the tightest TLS/heap margin — note largest-free-block logs during any sync.
- Any display artifact unique to the X3 panel (this is the least-tested profile post-migration).

---

# X4 Pro

## Preconditions
- [ ] Flashed current-tree build — **must include the dirty `freeink-sdk` submodule**
      (powerOffIdle reimplementation). Verify by checking serial for the UC8179
      POF path, or by the absence of the old page-turn noise.
- [ ] Serial attached; note whether `SETTINGS.fullTouchUi` starts off (it's opt-in).

## Boot & smoke
- [ ] Cold boot to Home, SD detected, heap > 50KB.
- [ ] SDK display parity: full/partial refresh + grayscale covers clean.

## Display power fixes (the headline X4 Pro items)
- [ ] **Page-turn noise**: turn 30+ pages — no audible/electrical noise regression
      (the earlier powerOffIdle *stub* caused noise; the reimplementation must not).
- [ ] **Static-screen speckle**: leave the device on a static screen (Home or a
      book page), untouched. At ~5 min the **global maintenance full repaint**
      should fire (visible full refresh). Leave ≥30 min total — **no speckle
      accumulates** on any static screen, reader or not.
- [ ] Confirm the 5-min repaint does NOT fire while actively turning pages
      (activity should reset it) and does not wake the device from sleep.
- [ ] Sleep and wake — screen state clean on wake (POF with RAM retained).

## Touch — existing behavior (Full Touch OFF)
- [ ] **Tap no longer fires long-press** (stale `getHeldTime()` seed fix): rapid
      single taps 20× — every one registers as a tap/Confirm, never a long-press action.
- [ ] Tap-anywhere-is-Confirm works in ordinary menus; reader touch zones page correctly.

## Full Touch mode (opt-in; landed in `c139e9a3` / `e207694e`)
- [ ] Enable Full Touch in settings; setting persists across reboot.
- [ ] In each **touch-enabled activity** (Home, File browser, Library, Group browser,
      reader menu, chapter select, bookmarks, clippings, footnotes, settings lists,
      WiFi/network selection, OPDS browser, BookFusion browser, sort/context menus):
      - [ ] Tapping a row/button activates **that** item (hit-test), not the highlighted one.
      - [ ] Tapping empty space does nothing (tap-anywhere-Confirm is off here).
      - [ ] Highlight drawn by the theme matches what a tap at the same spot hits
            (hitTest/draw lockstep — look for off-by-one-row activations).
- [ ] **Modal fallback**: open a modal without tap hit-testing (e.g. a confirmation
      dialog) from a Full-Touch screen — tap-activates-the-highlighted-option comes
      **back** inside the modal, then reverts on close.
- [ ] Home-key-tap-is-Confirm still works on Full-Touch screens.
- [ ] Buttons still work everywhere with Full Touch on (touch is additive, not exclusive).
- [ ] Toggle Full Touch off — all screens revert to tap-anywhere-Confirm.

## Core reading (fresh cache)
- [ ] Large EPUB: build + render, 20+ page turns (buttons AND touch zones), no artifacts.
- [ ] Chapter select, footnotes, bookmarks, progress restore, debounced writes.
- [ ] TXT + XTC readers; bionic toggle; grayscale images.

## Clippings
- [ ] Clip selection on a long chapter (with touch and with buttons) — no OOM; save/list/delete.

## UI changes
- [ ] Status bar per-element Hide/L/M/R + bookmark icon.
- [ ] Reader menu revamp: two-line summary, removed rows, visibility toggles.
- [ ] Tags/Authors browse; Font & Layout preview.

## Orientations (all 4)
- [ ] Home, reader, menus correct in all 4 modes.
- [ ] **Touch coordinates track orientation**: taps land on the right element in
      all 4 modes, with Full Touch both off and on.

## Network & sync
- [ ] WiFi + web server upload; OPDS; BookFusion sync (wolfSSL) completes.
- [ ] KOReader progress sync completes (pre-wolfSSL esp-tls path — regression check).
- [ ] Sync-timeout ghosting cleanup refresh works.
- [ ] Autosync silent push; autosync ⟷ BT mutual exclusion both directions.
- [ ] Cold boot → Library/Tags during recache with WiFi — no crash (3×).
- [ ] OTA check works.

## Power & sleep
- [ ] Auto-sleep fires; short power press wakes from deep sleep.
- [ ] **Battery-only** sleep paint: FULL_REFRESH, no ghosting (USB unplugged).
- [ ] Battery percentage plausible.

## Bluetooth (X4 Pro had device-specific fixes — test thoroughly)
- [ ] **Pairing-screen freeze fix**: open Bluetooth settings while a previously
      paired remote is advertising nearby — screen stays responsive, **no freeze**
      (fast-path reconnect must be gated to the reader only).
- [ ] Fresh pairing succeeds first try (`connectToDevice` double-timeout fix) —
      note if it needs a retry.
- [ ] AB Shutter3 + two-button signature wizard; page turns in reader.
- [ ] Reconnect inside the reader is fast (fast-path applies there).
- [ ] Auto-sleep still fires with an idle paired remote nearby.
- [ ] BLE torn down outside reader/settings; chapter build with BLE on succeeds.
- [ ] BLE auto-restore after build/sync — no freeze.
- [ ] 80MHz floor with BLE — no silent freeze; touch remains responsive with BLE up.

## Watch-for
- Speckle returning on any *new* static screen not covered by the maintenance
  repaint (it's global in main.cpp now, but note any screen that bypasses it).
- Any touch screen where highlight and tap disagree — record the activity name;
  that's a theme hitTest/draw lockstep break.
- Heap with touch + BLE + a large book simultaneously (worst-case RAM pressure device).
