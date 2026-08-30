#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)

// As silentRestartToReader(), but also flags that the restart was taken to win
// back a contiguous block for the BLE controller. On the next boot setup()
// re-arms "Bluetooth wanted", so the reader's maybeAutoRestoreBluetooth() brings
// the stack up by itself once a chapter is resident -- the user gets the remote
// back without touching the toggle again.
void silentRestartToReaderForBluetooth();

// False once a BLE-recovery restart has already been taken this boot. The guard
// is what stops a reboot loop when the heap is clean and enable() still fails
// for some other reason.
bool bluetoothRestartAvailable();
