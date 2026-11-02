#include "PowerHistory.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <limits>

#ifdef SIMULATOR
#include <cstdlib>
#endif

#include "util/TimeUtils.h"

namespace {
// A graph point is stored each time the level has fallen this far.
constexpr uint8_t SAMPLE_DROP_STEP = 5;
// A rise this large after USB was seen means a charge finished; a larger one
// with no USB seen means the device was charged while asleep.
constexpr uint8_t OBSERVED_CHARGE_RISE = 2;
constexpr uint8_t INFERRED_CHARGE_RISE = 8;

struct RuntimeState {
  unsigned long lastUpdateMs = 0;
  uint32_t batterySeconds = 0;
  uint32_t secondsSinceUnplug = 0;
  uint32_t batteryPageTurns = 0;
  uint32_t pageTurnsSinceUnplug = 0;
  bool started = false;
  bool externalPower = false;
  bool sawExternalPower = false;
  bool unpluggedThisSession = false;
};

RuntimeState runtime;

uint32_t saturatingAdd(const uint32_t lhs, const uint32_t rhs) {
  if (rhs > std::numeric_limits<uint32_t>::max() - lhs) {
    return std::numeric_limits<uint32_t>::max();
  }
  return lhs + rhs;
}

void accrueBatteryTime(const unsigned long now) {
  if (!runtime.started) return;

  if (runtime.externalPower) {
    runtime.lastUpdateMs = now;
    return;
  }

  const uint32_t elapsedSeconds = static_cast<uint32_t>((now - runtime.lastUpdateMs) / 1000UL);
  if (elapsedSeconds == 0) return;

  runtime.batterySeconds = saturatingAdd(runtime.batterySeconds, elapsedSeconds);
  if (runtime.unpluggedThisSession) {
    runtime.secondsSinceUnplug = saturatingAdd(runtime.secondsSinceUnplug, elapsedSeconds);
  }
  runtime.lastUpdateMs += elapsedSeconds * 1000UL;
}

// Seconds on battery this session, including the stretch since the last accrual.
uint32_t pendingBatterySeconds() {
  if (!runtime.started) return 0;
  if (runtime.externalPower) return runtime.batterySeconds;
  return saturatingAdd(runtime.batterySeconds, static_cast<uint32_t>((millis() - runtime.lastUpdateMs) / 1000UL));
}

bool currentLocalDay(uint32_t& outDay) {
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (now == 0) return false;
  outDay = TimeUtils::getLocalDayOrdinal(now);
  return true;
}

uint16_t activeMinutes(const uint32_t seconds) {
  return static_cast<uint16_t>(std::min<uint32_t>(seconds / 60U, UINT16_MAX));
}

void appendSample(PowerHistoryState& history, const uint8_t percent) {
  const uint16_t minutes = activeMinutes(history.activeSeconds);
  if (history.sampleCount > 0 && history.sampleActiveMinutes[history.sampleCount - 1] == minutes) {
    history.samplePercents[history.sampleCount - 1] = percent;
    return;
  }
  if (history.sampleCount >= PowerHistoryState::SAMPLE_CAPACITY) return;

  history.sampleActiveMinutes[history.sampleCount] = minutes;
  history.samplePercents[history.sampleCount] = percent;
  history.sampleCount++;
}

void stampCycleStartDay(PowerHistoryState& history) {
  uint32_t day = 0;
  if (!history.hasCycleStartDay && currentLocalDay(day)) {
    history.cycleStartDay = day;
    history.hasCycleStartDay = true;
  }
}

void startCycle(PowerHistoryState& history, const uint8_t percent, const uint32_t sessionSeconds,
                const uint32_t sessionPageTurns, const bool confirmed) {
  history = {};
  history.activeSeconds = sessionSeconds;
  history.readerPageTurns = sessionPageTurns;
  history.lastPercent = percent;
  history.cycleConfirmed = confirmed;
  stampCycleStartDay(history);
  appendSample(history, percent);
}

#ifdef SIMULATOR
const PowerHistoryState& demoHistory() {
  static const PowerHistoryState demo = [] {
    PowerHistoryState state;
    state.activeSeconds = (29U * 3600U) + (47U * 60U);
    state.readerPageTurns = 2840;
    state.hasCycleStartDay = true;
    state.cycleConfirmed = true;
    state.lastPercent = 35;

    constexpr uint16_t minutes[] = {0, 118, 241, 376, 515, 654, 798, 942, 1087, 1231, 1380, 1523, 1659, 1787};
    constexpr uint8_t percents[] = {100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 35};
    static_assert(sizeof(minutes) / sizeof(minutes[0]) == sizeof(percents) / sizeof(percents[0]));
    state.sampleCount = static_cast<uint8_t>(sizeof(minutes) / sizeof(minutes[0]));
    for (uint8_t i = 0; i < state.sampleCount; ++i) {
      state.sampleActiveMinutes[i] = minutes[i];
      state.samplePercents[i] = percents[i];
    }
    return state;
  }();
  return demo;
}
#endif
}  // namespace

namespace PowerHistory {
void begin(const bool externalPowerConnected) {
  runtime = {};
  runtime.started = true;
  runtime.externalPower = externalPowerConnected;
  runtime.sawExternalPower = externalPowerConnected;
  runtime.lastUpdateMs = millis();
}

void noteExternalPower(const bool connected) {
  if (!runtime.started) {
    begin(connected);
    return;
  }
  if (runtime.externalPower == connected) return;

  const unsigned long now = millis();
  accrueBatteryTime(now);
  runtime.externalPower = connected;
  runtime.lastUpdateMs = now;
  runtime.secondsSinceUnplug = 0;
  runtime.pageTurnsSinceUnplug = 0;
  runtime.unpluggedThisSession = !connected;
  if (connected) {
    runtime.sawExternalPower = true;
  }
}

void recordReaderPageTurn() {
  if (!runtime.started || runtime.externalPower) return;

  runtime.batteryPageTurns = saturatingAdd(runtime.batteryPageTurns, 1);
  if (runtime.unpluggedThisSession) {
    runtime.pageTurnsSinceUnplug = saturatingAdd(runtime.pageTurnsSinceUnplug, 1);
  }
}

void commitBeforeSleep(const bool externalPowerConnected) {
  if (!runtime.started) begin(externalPowerConnected);
  noteExternalPower(externalPowerConnected);
  accrueBatteryTime(millis());

  auto& history = APP_STATE.powerHistory;
  const uint16_t measuredPercent = powerManager.getBatteryPercentage();

  if (measuredPercent > 100) {
    history.activeSeconds = saturatingAdd(history.activeSeconds, runtime.batterySeconds);
    history.readerPageTurns = saturatingAdd(history.readerPageTurns, runtime.batteryPageTurns);
    stampCycleStartDay(history);
    if (externalPowerConnected && history.lastPercent <= 100) {
      history.chargePending = true;
      history.chargePendingPercent = history.lastPercent;
    }
    LOG_ERR("PWRH", "Battery sample unavailable at sleep; active time retained");
  } else {
    const uint8_t percent = static_cast<uint8_t>(measuredPercent);
    const bool hasPreviousSample = history.lastPercent <= 100;
    const uint8_t chargeBaseline = history.chargePending && history.chargePendingPercent <= 100
                                       ? history.chargePendingPercent
                                       : history.lastPercent;
    const bool chargeWasObserved = history.chargePending || runtime.sawExternalPower;
    const bool observedChargeCompleted = hasPreviousSample && !externalPowerConnected && chargeWasObserved &&
                                         chargeBaseline <= 100 && percent >= chargeBaseline &&
                                         percent - chargeBaseline >= OBSERVED_CHARGE_RISE;
    const bool inferredChargeCompleted = hasPreviousSample && !externalPowerConnected &&
                                         percent >= history.lastPercent &&
                                         percent - history.lastPercent >= INFERRED_CHARGE_RISE;
    const bool startsNewCycle = observedChargeCompleted || inferredChargeCompleted;

    if (!hasPreviousSample || startsNewCycle) {
      // Charged and unplugged within this session: only what came after the
      // unplug belongs to the new cycle.
      const bool sinceUnplug = startsNewCycle && runtime.unpluggedThisSession;
      startCycle(history, percent, sinceUnplug ? runtime.secondsSinceUnplug : runtime.batterySeconds,
                 sinceUnplug ? runtime.pageTurnsSinceUnplug : runtime.batteryPageTurns, startsNewCycle);
    } else {
      history.activeSeconds = saturatingAdd(history.activeSeconds, runtime.batterySeconds);
      history.readerPageTurns = saturatingAdd(history.readerPageTurns, runtime.batteryPageTurns);
      if (history.sampleCount == 0) {
        appendSample(history, percent);
      } else {
        const uint8_t storedPercent = history.samplePercents[history.sampleCount - 1];
        if (storedPercent >= SAMPLE_DROP_STEP && percent <= storedPercent - SAMPLE_DROP_STEP) {
          appendSample(history, percent);
        }
      }
      history.lastPercent = percent;
    }

    if (externalPowerConnected) {
      history.chargePendingPercent = history.chargePending && history.chargePendingPercent <= 100
                                         ? std::min(history.chargePendingPercent, percent)
                                         : percent;
      history.chargePending = true;
    } else {
      history.chargePending = false;
      history.chargePendingPercent = UINT8_MAX;
    }
    LOG_INF("PWRH", "Sleep sample=%u%% active=%lus points=%u", static_cast<unsigned>(percent),
            static_cast<unsigned long>(history.activeSeconds), static_cast<unsigned>(history.sampleCount));
  }

  // This session is now in the history. Deep sleep can still be refused (a
  // wake source already asserted), and a second commit must not add it twice.
  runtime.batterySeconds = 0;
  runtime.secondsSinceUnplug = 0;
  runtime.batteryPageTurns = 0;
  runtime.pageTurnsSinceUnplug = 0;
}

bool isDemoMode() {
#ifdef SIMULATOR
  static const bool enabled = [] {
    const char* value = std::getenv("CROSSPOINT_SIM_POWER_DEMO");
    return value && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
#else
  return false;
#endif
}

const PowerHistoryState& historyForDisplay() {
#ifdef SIMULATOR
  if (isDemoMode()) return demoHistory();
#endif
  return APP_STATE.powerHistory;
}

uint32_t activeSecondsForDisplay() {
  if (isDemoMode()) return historyForDisplay().activeSeconds;
  return saturatingAdd(APP_STATE.powerHistory.activeSeconds, pendingBatterySeconds());
}

uint32_t pageTurnsForDisplay() {
  if (isDemoMode()) return historyForDisplay().readerPageTurns;
  return saturatingAdd(APP_STATE.powerHistory.readerPageTurns, runtime.batteryPageTurns);
}

bool elapsedDaysForDisplay(uint32_t& outDays) {
  if (isDemoMode()) {
    outDays = 10;
    return true;
  }
  const auto& history = APP_STATE.powerHistory;
  if (!history.hasCycleStartDay) return false;

  uint32_t today = 0;
  if (!currentLocalDay(today) || today < history.cycleStartDay) return false;
  outDays = today - history.cycleStartDay;
  return true;
}
}  // namespace PowerHistory
