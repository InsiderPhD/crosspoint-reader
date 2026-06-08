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
//   1. Looks up the last-connected SSID from WIFI_STORE
//   2. Reconnects silently (no UI), waits up to ~5s for association
//   3. On success, retries NTP up to 3 times for a fresh wall clock
//   4. Tears WiFi back down regardless of outcome
// No-op if the clock is already valid this boot, or if there is no saved
// last-connected network. Safe to call from main.cpp's setup() — does not
// block boot because the work runs on a FreeRTOS task.
void startSilentBootAttempt();

}  // namespace WifiTimeSync
