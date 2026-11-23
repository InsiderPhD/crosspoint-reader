#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();            // home screen
void silentRestartToReader();    // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToSettings();  // settings screen
// Reboots straight after an activity hands exclusive raw storage back (USB
// Drive), without touching the SD card, and lands on Home.
void restartToHomeAfterStorageHandoff();
