#pragma once

// The one sleep sequence: BLE teardown, APP_STATE save, sleep screen, then every
// peripheral parked (frontlight, tilt sensor, panel) before the MCU powers down.
// Defined in main.cpp. Every sleep entry point must come through here — a copy
// that skips a parking step leaves that part powered for the whole sleep.
void enterDeepSleep(bool fromTimeout = false);
