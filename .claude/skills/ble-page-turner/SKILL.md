---
name: ble-page-turner
description: Bluetooth LE page-turner remotes on the ESP32-C3 (NimBLE central, HID clickers). Use when touching lib/hal/BluetoothHIDManager, lib/hal/DeviceProfiles, BluetoothSettingsActivity, the reader's Bluetooth toggle, or anything that shares the radio (WiFi sync) or the heap (section builds, TLS) with BLE. Covers the vendored-NimBLE constraint, the heap budget that actually decides whether enable() works, remote dialect decoding, and the traps that cost a full debugging night.
---

# BLE Page Turner (ESP32-C3)

Two things break this feature, and neither is the Bluetooth code: **the heap**
and **remote dialect diversity**. Everything below is measured on hardware, not
inferred from docs.

## Hard constraints (do not re-derive these)

- **`CONFIG_BT_NIMBLE_*` build flags DO NOTHING.** arduino-esp32 3.x hard-defines
  them in the framework's `sdkconfig.h`, which is included after the command line
  and wins. Symptom: `warning: "CONFIG_BT_NIMBLE_X" redefined`. NimBLE-Arduino is
  therefore **vendored in `lib/NimBLE-Arduino`** with patches marked
  `CrossPoint (vendored patch)` — mbuf pools, host task stack, controller config.
  Trim there or not at all.
- **The BLE stack costs ~50-56KB of heap in several large chunks.** The controller
  needs a **≥30KB contiguous** block; it fails with `BLE_INIT: Malloc failed` on a
  fragmented heap even when total free is 85-95KB. `enable()`'s gate currently
  checks total free only (75KB) — **it must also check
  `ESP.getMaxAllocHeap() ≥ ~30KB`**, or it green-lights an enable that then fails.
- **`NimBLEDevice::init()` returns bool and the current code ignores it** —
  `enable()` reports "Bluetooth enabled successfully" after a failed init. Check it.
- **mbedtls record buffers are fixed at 16KB in+out** (`MBEDTLS_SSL_MAX_CONTENT_LEN`,
  precompiled). Every TLS session needs 2×16KB contiguous *plus* X509/RSA working
  space. `esp_http_client`'s `buffer_size` only shrinks the HTTP layer — it does
  not touch this.
- **X3 vs X4**: X3 framebuffer is 52.3KB (792×528) vs X4's 48KB (800×480), so X3
  sits ~4KB closer to every cliff. X4 may additionally allocate a 48KB
  `_asyncShadow` in FreeInkDisplay that X3 never does.

## Before enabling BLE (or opening TLS), free the heap

The reader holds the memory BLE needs. Release in this order, then reload from
cache afterwards:

1. `section.reset()` (cache the page/count into `nextPageNumber` /
   `cachedChapterTotalPageCount` first)
2. `epub.reset()` — spine/TOC strings are large for omnibus books; reload with
   `make_shared<Epub>(path, "/.crosspoint")` + `load(...)`, `onGoHome()` on failure
3. `renderer.getFontCacheManager()->clearCache()` — re-warms on next render

`performBookFusionSync()` does all three and is the reference implementation.
**`toggleBluetoothFromReader()` currently does only 1 and 3** — omitting the
`epub` release is what left ~5-10KB free and OOM-aborted the next render on the
X3 (`FDC Failed to allocate temp buffer` → `abort()`). The KOReader quick-sync
path also lacks the `epub` release. Both are known gaps, not deliberate.

## Radio and lifecycle rules

- **BLE and WiFi share one radio.** Disable BLE before any WiFi session.
- **Never enable BLE immediately after a WiFi session.** `esp_wifi_stop()` →
  `esp_bt_controller_init()` **hard-freezes the device**, even with healthy heap
  and a 500ms settle delay. Auto-restore after sync was built, tested, and
  removed. Only re-enable from a user action, with no WiFi in between.
- **Enabling is manual, by design.** BLE runs only in the reader and the Bluetooth
  settings screen (`Activity::keepsBluetoothActive()`), and shuts down for section
  builds (`BleMemoryPause`), syncs, leaving the reader, and sleep. It never turns
  itself back on.
- **`disable()` must use `NimBLEDevice::deinit(true)`.** `deinit(false)` keeps
  client objects alive scattered through the freed region and caps the largest
  free block at ~20KB. **Consequence:** every `setClientCallbacks()` /
  `setScanCallbacks()` must pass `deleteCallbacks = false` for static callback
  objects, or destruction calls `free()` on a static → `heap_caps_free` assert.

## Decoding remotes (they are all different)

Cheap clickers do not agree on anything. `DeviceProfiles` handles three shapes:

- **Standard**: distinct HID keycode per button at a fixed report byte.
- **Positional**: *same* keycode (e.g. `0x07`) for every button; identity lives in
  *which byte* carries it. Handled by **index-locked profiles**
  (`pageUpIndex`/`pageDownIndex`, `0xFF` = keycode-only matching), which decode to
  the internal sentinels `LEARNED_BACK_CODE` / `LEARNED_FORWARD_CODE`.
- **One-button toggle**: a single button alternating between two codes per press.
  `oneButtonMode` maps *both* learned codes to page-forward.

The setup wizard captures **raw HID frames** (not decoded keycodes), accumulates
~3 frames per press to mask unstable bytes (rolling counters, joystick axes), then
diffs forward-vs-back frames to derive per-direction `(byteIndex, value)`. A
single captured direction falls back to a stable-byte heuristic.

**Known bug (unfixed):** the global learned profile is what drives decoding, so
mapping a second remote clobbers the first. Per-device profiles exist but only
feed `simpleFallback`. Fix = make per-device entries carry the full index-locked
mapping and have the wizard save only to the device it learned from.

## Input injection

Remote keys become **virtual presses of physical buttons** via
`HalGPIO::setVirtualButtonState()`, so activities need no BLE awareness.

- **Presses shorter than one loop iteration must survive.** `pendingVirtualPresses`
  latches every injected press for ≥1 frame; without it a clicker that sends
  press+release 1ms apart is invisible. This bug looks exactly like "decoding
  works but nothing happens".
- Long-press logic must use **per-button** `getHeldTime(button)`, never the global
  `getHeldTime()`, or a stale physical hold time gets attributed to a virtual press.
- The wizard sets `setInjectionSuppressed(true)` so the remote can't drive the menu
  it's being mapped on.

## Reconnecting

A disconnected remote advertises only after **its own** button is pressed. A
passive low-duty background scan (interval 1600 / window 80) watches for the
bonded MAC and sets `_pendingBondedConnect`, consumed in the loop task. Physical
device presses also trigger a reconnect attempt. Never call `connectToDevice()`
from the NimBLE callback context.

Worth adding: `setMaxResults(0)` on both scans — the app keeps its own device
list, so NimBLE's parallel cache of `NimBLEAdvertisedDevice` objects is pure
waste in a busy RF environment.

## Debugging procedure

Read the logs in this order — they discriminate the failure modes:

| Symptom | Look for | Meaning |
|---|---|---|
| Enable fails/hangs | `Enable attempt: heap N, max alloc M` | M < 30KB ⇒ fragmentation, not shortage |
| Pages don't turn | `keycode=0x.., pressed=1` present? | Decode OK ⇒ injection/latch bug, not BLE |
| Every other press ignored | `Unmatched frame [N]: XX ..` | Toggle remote ⇒ `oneButtonMode` |
| Wrong/no buttons | `Using learned custom profile: up=0x..@i dn=0x..@j` | Compare against the actual frames |
| Device won't boot at all, no serial | (nothing) | See NVS trap below |

`heap_caps_print_heap_info(MALLOC_CAP_8BIT)` gives the region map (free vs largest
block per region); `heap_caps_dump()` names individual blocks.

## Tried and rejected (don't re-run these)

- **Auto re-enabling BLE after a sync** — froze the device, twice, even with a
  500ms radio settle. The WiFi→BT controller handoff is the hazard, not the heap.
- **Trimming NimBLE via `-D CONFIG_BT_NIMBLE_*`** — silently overridden (see above).
- **A static 32KB BSS pool for the inflate dictionary** — made section builds
  fragmentation-proof, but the permanent 32KB pushed "reader + BLE" over the
  budget (4KB free → OOM abort). Reverted.
- **Migrating BookFusion sync to `esp_http_client` with 2KB buffers** — the buffer
  setting doesn't reach mbedtls, and it forced a CA-verification path the
  framework can't satisfy. Reverted to `WiFiClientSecure` + `setInsecure()`,
  keeping the good parts (one reused connection, real timeouts).
- **Boot-time BLE reservation** (init the stack in `setup()` on a clean heap, make
  the user toggle a radio-only switch) — designed and written, reverted before
  hardware proof. Still the most promising answer if mid-session enables on X3
  need solving; trade is ~50KB less free heap for all bonded users, and syncs
  still need the stack torn down.

## Traps that cost real time

- **A crash mid-NVS-write corrupts NVS and bricks boot** — no serial, no display,
  every reflash appears to hang. Cure: `esptool.py erase_region 0x9000 0x5000`.
  Suspect this whenever *known-good* firmware also fails to boot.
- **Wiping NVS drops BLE bonds.** Reconnect still works (MAC is on SD) but if the
  remote refuses, Forget + re-pair.
- **`bookfusion.com` TLS**: the framework CA bundle fails its Sectigo chain
  ("certificate matched but signature verification failed"), and framework esp-tls
  **forbids** no-CA configs; pinned roots additionally need a correct clock (leaf
  `notBefore` is recent). The client uses `WiFiClientSecure::setInsecure()`.
- **Giant `GFX Time = N ms` values during sync are a logging artifact** (timer runs
  from the last `clearScreen`), not slow rendering.
