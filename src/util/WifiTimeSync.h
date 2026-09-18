#pragma once

#include <cstdint>

// Single entry point for "we just used WiFi, take the chance to fix the clock"
// behaviour. The ESP32-C3 has no battery-backed RTC, so the system clock resets
// every cold boot. Without NTP, reading-stats fall back to a stale
// lastKnownValidTimestamp and attribute every read to that day — which is the
// bug class step 3 of the date-correctness plan is closing.
//
// Hook this from anywhere a WiFi connection is established (WifiSelection
// onComplete, web server startup, BookFusion/KOReader sync) — it's a no-op once
// the boot's clock is already valid, and on success it both sets the system
// clock (via SNTP) AND persists APP_STATE.lastKnownValidTimestamp.
namespace WifiTimeSync {

// Try once. Returns true if the wall clock is valid after this call (whether
// from this attempt or a previous successful sync this boot). Safe to call when
// WiFi is not connected — returns false without side effects.
//
// timeoutMs bounds the SNTP wait. Default 8s — corporate networks routinely
// take >5s for DNS + first SNTP response, so the tighter window was leaving
// users stuck on a stale lastKnownValidTimestamp even after a successful
// WiFi connect.
bool attemptIfStale(uint32_t timeoutMs = 8000);

// Fire-and-forget silent boot-time NTP attempt. Spawns a background task that:
//   1. Waits 1.5s so the first paint isn't fighting it for SD/SPI bandwidth
//   2. Looks up the last-connected SSID from WIFI_STORE
//   3. Reconnects silently (no UI), waiting up to 8s for association — but
//      stopping ~2s after the driver reports the network is absent or rejected
//      us, so a device booting away from its saved network doesn't hold the
//      radio open for the full budget on every boot
//   4. On association, up to 2 SNTP attempts at 5s each
//   5. Tears WiFi back down regardless of outcome
// No-op if the clock is already valid this boot, or if there is no saved
// last-connected network. Safe to call from main.cpp's setup() — does not
// block boot because the work runs on a FreeRTOS task.
//
// Worst case the radio is up for ~18s (8s association + 10s SNTP), starting
// 1.5s after setup() returns. That overlaps the first activity's own work, and
// the WiFi stack holds ~40KB throughout, so treat it as part of the boot
// budget — the task logs its own duration for exactly this reason.
void startSilentBootAttempt();

// Cooperatively stop the silent boot NTP task and wait (bounded) until it has
// released the WiFi radio — which frees the ~40KB the WiFi stack holds. Call
// this before a heavy, allocation-hungry foreground job (e.g. the library
// metadata recache) so the two don't contend for heap or the SD/SPI bus and
// crash on a cold boot. No-op (returns immediately) if the task isn't running,
// so once the boot window above has elapsed this costs nothing. Safe to call
// from any task.
//
// maxWaitMs caps the wait: teardown is usually <200ms (the task polls WiFi
// association every 100ms), but a browse that lands mid-SNTP may wait up to one
// SNTP timeout (~5s) + teardown before the task notices the stop request — the
// 8s default leaves headroom so preempt() never gives up with WiFi still up.
void preempt(uint32_t maxWaitMs = 8000);

}  // namespace WifiTimeSync
