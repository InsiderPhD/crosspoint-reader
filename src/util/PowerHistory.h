#pragma once

#include <cstdint>

#include "CrossPointState.h"

// Battery level against awake-on-battery time for the current charge cycle
// (ported from YACP's Autonomy tracking). The battery is sampled once, at
// deep-sleep entry, and stored in state.json by the save that already happens
// there: no periodic wake-ups, no extra card writes, no heap.
namespace PowerHistory {

// Starts the in-RAM awake-time accumulator. Touches neither battery nor card.
void begin(bool externalPowerConnected);

// Call on the USB plug/unplug edge. Time on external power is not counted.
void noteExternalPower(bool connected);

// Counts one reader page turn made on battery. RAM only until the next sleep.
void recordReaderPageTurn();

// Folds this awake session into APP_STATE.powerHistory and takes at most one
// battery sample. Call immediately before enterDeepSleep()'s state save.
void commitBeforeSleep(bool externalPowerConnected);

// The simulator can show a fixed demo cycle (CROSSPOINT_SIM_POWER_DEMO=1).
bool isDemoMode();
const PowerHistoryState& historyForDisplay();
// Stored totals plus what this awake session has added so far.
uint32_t activeSecondsForDisplay();
uint32_t pageTurnsForDisplay();
// Calendar days since the cycle started. False without a trustworthy clock.
bool elapsedDaysForDisplay(uint32_t& outDays);

}  // namespace PowerHistory
