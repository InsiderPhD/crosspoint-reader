---
name: ble-page-turner
description: Bluetooth LE page-turner remotes (NimBLE central, HID clickers). Use when touching lib/hal/BluetoothHIDManager, BluetoothSettingsActivity, lib/DevicePolicy, the reader's Bluetooth toggle/auto-restore, or anything that shares the radio (WiFi sync) or the heap (section builds, TLS) with BLE. Covers the vendored-NimBLE constraint, the heap budget that decides whether enable() works on the C3 boards, the structural press detector, and the traps that cost a full debugging night.
---

# BLE Page Turner

Two things break this feature, and neither is the Bluetooth code: **the heap**
and **remote dialect diversity**. Everything below is measured on hardware, not
inferred from docs.

**Device classes matter.** `lib/DevicePolicy/DevicePolicy.h` keys behaviour on
PSRAM presence: the ESP32-C3 boards (X3: 792×528, 52.3KB framebuffer; X4:
800×480, 48KB) are `CROSSPOINT_TIGHT_HEAP` and every constraint in this file
applies to them in full. The X4 Pro (ESP32-S3, 512KB SRAM + 8MB PSRAM,
radio coexistence) is exempted via `CROSSPOINT_BLE_EXCLUSIVE=0` — BLE stays up
through WiFi sessions, chapter builds, and image decodes there. When editing
BLE lifecycle code, check which side of the policy `#if` you are on.

## Hard constraints on the C3 (do not re-derive these)

- **`CONFIG_BT_NIMBLE_*` build flags DO NOTHING.** arduino-esp32 3.x hard-defines
  them in the framework's `sdkconfig.h`, which is included after the command line
  and wins. Symptom: `warning: "CONFIG_BT_NIMBLE_X" redefined`. NimBLE-Arduino is
  therefore **vendored in `lib/NimBLE-Arduino`** with patches marked
  `CrossPoint (vendored patch)` — mbuf pools, host task stack, controller config.
  Trim there or not at all.
- **The BLE stack costs ~50-56KB of heap in several large chunks**, and the
  controller needs a **≥30KB contiguous** block. On a fragmented heap it can
  fail — or **hang inside controller init** — even with 85-99KB total free
  (measured: free=99316 / maxAlloc=26612 after one BLE cycle could not start).
  `enable()` therefore gates on BOTH `ESP.getFreeHeap() ≥ 75KB` AND
  `ESP.getMaxAllocHeap() ≥ BLE_CONTROLLER_MIN_BLOCK` (20KB floor; above it we
  attempt and log `Enable attempt: heap N, largest block M` so the real
  requirement can be learned from the field). **Both checks are load-bearing —
  the contiguity one is the one that actually bites. Never remove either.**
- **`NimBLEDevice::init()`'s bool return is checked** and maps to
  `BtStatus::StartFailed`. Every enable/connect failure sets
  `BluetoothHIDManager::lastStatus`; the UI translates it via `btStatusText()`
  in BluetoothSettingsActivity (the manager sits below I18n by design). Route
  any new failure mode through `lastStatus`, not ad-hoc strings.
- **TLS**: both sync clients (BookFusion, KOReader) now run on
  `freeink::SecureHttpClient` (wolfSSL, TLS 1.3, X25519) — the old framework
  mbedTLS with its fixed 2×16KB contiguous record buffers is gone. The wolfSSL
  handshake is far leaner, but the free-the-heap discipline below is
  **deliberately retained** until the new headroom is measured on-device with
  BLE up. Don't strip it on the strength of wolfSSL alone.
- **X3 sits ~4KB closer to every cliff than X4** (52.3KB vs 48KB framebuffer) —
  BUT any non-X3 panel (the C3 X4 included) lazily mallocs a 48KB
  `_asyncShadow` in FreeInkDisplay on its first async refresh and keeps it
  resident (X3 takes the blocking display path and never allocates it; a failed
  allocation falls back to blocking, so it degrades gracefully). On the C3 X4
  that shadow is a real 48KB bite out of the same heap BLE wants; on the X4 Pro
  PSRAM absorbs it.

## Before enabling BLE (or opening TLS) on the C3, free the heap

The reader holds the memory BLE needs. Release in this order, then reload from
cache afterwards:

1. `section.reset()` (cache the page/count into `nextPageNumber` /
   `cachedChapterTotalPageCount` first)
2. `epub.reset()` — spine/TOC strings are large for omnibus books; with them
   resident the controller's chunks interleave and the post-enable heap is
   shredded (~5-10KB free, no glyph-buffer-sized block → OOM abort on the X3)
3. `renderer.getFontCacheManager()->clearCache()` — re-warms on next render

**Every current path does all three**: `toggleBluetoothFromReader()`,
`maybeAutoRestoreBluetooth()`, the autosync push, and the manual BookFusion
sync. The reload helper is `reloadEpubAfterBluetooth()` (borrows the
framebuffer as inflate scratch via `InflateScratchLease`; `onGoHome()` on
failure). **Invariant: any new path that enables BLE or opens TLS from the
reader must do the same three releases.**

## Radio and lifecycle rules

- **On the C3, BLE and WiFi are mutually exclusive** (`CROSSPOINT_BLE_EXCLUSIVE`):
  `enable()` powers WiFi down first, and syncs tear the BLE stack down before
  WiFi comes up. Not applicable on the X4 Pro (software coexistence).
- **Enabling the BT controller right after `esp_wifi_stop()` hard-froze the C3**,
  twice, even with healthy heap and a 500ms settle delay. The naive auto-restore
  was removed because of this. The **current auto-restore is gated**: the user's
  intent is recorded via `setBluetoothWanted()`, and the reader's idle loop
  (`maybeAutoRestoreBluetooth()`) re-enables only when a chapter+epub are
  resident to free, ≥3s after the last page turn, and only after the manager's
  `beginAutoRestoreAttempt()` gate — which enforces `BLE_WIFI_SETTLE_MS` (3s)
  since the last `noteWifiActivity()`. This gated version is **not yet
  hardware-proven**; if a device wedges after a sync, suspect the WiFi→BT
  handoff first.
- **BLE runs only in the reader and the Bluetooth settings screen**
  (`Activity::keepsBluetoothActive()`), and shuts down for section builds
  (`BleMemoryPause`), syncs, leaving the reader, and sleep. It comes back only
  via the gated auto-restore above or a user action.
- **`disable()` must use `NimBLEDevice::deinit(true)`** (it does). `deinit(false)`
  keeps client objects alive scattered through the freed region and caps the
  largest free block at ~20KB. **Consequence:** every `setClientCallbacks()` /
  `setScanCallbacks()` must pass `deleteCallbacks = false` for static callback
  objects, or destruction calls `free()` on a static → `heap_caps_free` assert.

## Decoding remotes (they are all different)

Cheap clickers do not agree on anything. The old `DeviceProfiles` keycode tables
are **gone** — decoding is now a **structural press detector** in
`BluetoothHIDManager` (`detectPress`):

- On connect, the manager **learns each report's idle baseline** (~`BASELINE_LEARN_MS`)
  and a volatile-byte mask (rolling counters, joystick axes). Any unmasked
  deviation from idle = a press; re-masking handles reports that churn.
- **A byte counts as "rests at zero" (keycode, idle forced to 0, never masked)
  only if it was 0x00 for ≥¼ of the learn-window frames** (`byteZeroCount`,
  2026-08-18). The old any-single-zero-visit rule misclassified the high byte of
  a 16-bit joystick axis transiting below 0x100 (idle 0x01F4 → transient 0x00C8),
  inverting the detector for the session. Smoking gun in logs: a press sig whose
  value equals the byte's steady-state value (`sig 5=0x01`).
- Each press records a **signature**: the first unmasked byte that left idle,
  plus its value (`lastPressSignature()`). That's what the **wizard** captures.
- Mapping is a signature pair in settings (`SETTINGS.bleBackSigIndex/Value`,
  `bleFwdSigIndex/Value`, `0xFF` = unmapped), pushed to the manager at boot via
  `setButtonMapping()`. **Decode matches the back signature against the edge
  frame's CONTENT** (`frame[backIdx]==backVal` + left-idle-or-masked guard,
  `lastPressIsBack`), NOT against the first-deviating byte — the learned mask is
  session-dependent (depends on what traffic flowed in the 1s window), so the
  first-deviating index can flip between the wizard's session and a later one
  (that was the "back also pages forward" bug, 2026-08-18). Match → PageBack;
  **anything else → PageForward**. Unmapped = every press pages forward, which
  is the right default for one-button clickers. Mappings stored before
  2026-08-18 were learned under the zero-visit bug — re-run the wizard.
- The wizard (BluetoothSettingsActivity) requires **two matching presses per
  direction**. A mismatch within a step means the button alternates codes per
  press (one-button toggle remote) — mapping is cleared with an explanation and
  everything pages forward. Forward==back clears too.
- **One bonded remote at a time**; Forget clears the bond AND the mapping, so
  the old "second remote clobbers the first" bug died with `DeviceProfiles`.
- **Guided first-time setup** (added 2026-08-15, not yet hardware-tested): the
  "No remote - connect one" row auto-enables Bluetooth (specific `BtStatus`
  reason on failure), scans, pairs, chains straight into the mapping wizard
  (Back reads Skip there and clears any stale mapping), and lands on the
  main-menu test box.

## Input injection

Remote presses become **virtual presses of physical buttons** via
`HalGPIO::setVirtualButtonState()` (PageBack/PageForward resolved through
`MappedInputManager`), so activities need no BLE awareness.

- **Presses shorter than one loop iteration must survive.** `pendingVirtualPresses`
  latches every injected press for ≥1 frame; without it a clicker that sends
  press+release 1ms apart is invisible. This bug looks exactly like "decoding
  works but nothing happens".
- Long-press logic must use **per-button** `getHeldTime(button)`, never the global
  `getHeldTime()`, or a stale physical hold time gets attributed to a virtual press.
- The wizard sets `setInjectionSuppressed(true)` so the remote can't drive the menu
  it's being mapped on (cleared in `endButtonMapping` AND `onExit`).

## Reconnecting

A disconnected remote advertises only after **its own** button is pressed. A
passive low-duty background scan (interval 1600 / window 80) watches for the
bonded MAC and sets `_pendingBondedConnect`, consumed in the loop task. Physical
device presses also trigger a reconnect attempt. Never call `connectToDevice()`
from the NimBLE callback context.

Both scans run `setMaxResults(0)` — the app keeps its own device list, so
NimBLE's parallel cache of `NimBLEAdvertisedDevice` objects would be pure waste
in a busy RF environment. Keep it that way.

## Debugging procedure

Read the logs in this order — they discriminate the failure modes:

| Symptom | Look for | Meaning |
|---|---|---|
| Enable fails/hangs | `Enable attempt: heap N, largest block M` | M < ~30KB ⇒ fragmentation, not shortage |
| No presses at all | `[BTDBG] addr=.. raw=.. mask=..` (auto-enabled on the BT settings screen) | frames absent ⇒ link/subscribe problem |
| Frames arrive, no page turn | `>>> REMOTE PRESS -> page forward/back <<<` | present ⇒ injection/latch bug, not BLE |
| Wrong direction | `Button mapping: back sig i=0x.., fwd sig j=0x..` | compare against `Remote press captured (sig ..)` |
| Device won't boot at all, no serial | (nothing) | See NVS trap below |

`heap_caps_print_heap_info(MALLOC_CAP_8BIT)` gives the region map (free vs largest
block per region); `heap_caps_dump()` names individual blocks.

## Tried and rejected (don't re-run these)

- **Auto re-enabling BLE right after a sync with no settle gate** — froze the
  device, twice, even with a 500ms radio settle. The WiFi→BT controller handoff
  is the hazard, not the heap. Superseded by the gated auto-restore above
  (`BLE_WIFI_SETTLE_MS`), which is itself unproven on hardware — do not loosen
  its gates.
- **Trimming NimBLE via `-D CONFIG_BT_NIMBLE_*`** — silently overridden (see above).
- **A static 32KB BSS pool for the inflate dictionary** — made section builds
  fragmentation-proof, but the permanent 32KB pushed "reader + BLE" over the
  budget (4KB free → OOM abort). Reverted; the framebuffer-borrow leases
  (`InflateScratchLease` etc., see DevicePolicy.h) are the surviving answer.
- **Migrating sync to `esp_http_client` with 2KB buffers** — the buffer setting
  never reached mbedtls's fixed 16KB records, and it forced a CA-verification
  path the framework can't satisfy. That whole saga ended in the wolfSSL
  migration (`freeink::SecureHttpClient`); don't revisit esp_http_client.
- **Boot-time BLE reservation** (init the stack in `setup()` on a clean heap, make
  the user toggle a radio-only switch) — designed and written, reverted before
  hardware proof. Still the most promising answer if mid-session enables on X3
  need solving; trade is ~50KB less free heap for all bonded users, and syncs
  still need the stack torn down. Moot on the X4 Pro.

## Traps that cost real time

- **A crash mid-NVS-write corrupts NVS and bricks boot** — no serial, no display,
  every reflash appears to hang. Cure: `esptool.py erase_region 0x9000 0x5000`.
  Suspect this whenever *known-good* firmware also fails to boot.
- **Wiping NVS drops BLE bonds.** Reconnect still works (MAC is on SD) but if the
  remote refuses, Forget + re-pair.
- **`bookfusion.com` TLS**: the framework CA bundle fails its Sectigo chain
  ("certificate matched but signature verification failed"), and pinned roots
  additionally need a correct clock (leaf `notBefore` is recent). That is why
  the wolfSSL client runs `setInsecure()` — the project's historical trust
  model. Don't "fix" verification without solving both.
- **Giant `GFX Time = N ms` values during sync are a logging artifact** (timer runs
  from the last `clearScreen`), not slow rendering.
