#include "ActivityManager.h"

#include <BluetoothHIDManager.h>
#include <HalPowerManager.h>

#include "SdCardFontGlobals.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/ActionBar.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/GroupBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/LibraryActivity.h"
#include "home/ReadingHeatmapActivity.h"
#include "home/ReadingStatsActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FullScreenMessageActivity.h"

namespace {
// Guards waitingTaskHandle against concurrent access by the render task and the
// task blocked in requestUpdateAndWait().
//
// This MUST be a real spinlock. These sites previously passed nullptr, which the
// single-core RISC-V port (ESP32-C3) silently accepts — it only disables
// interrupts and ignores the argument entirely. The dual-core Xtensa port
// (ESP32-S3, i.e. the X4 Pro) genuinely needs the spinlock to serialise the two
// cores, and asserts on a null one:
//     assert failed: spinlock_acquire spinlock.h:84 (lock)
// which took down the render task on every boot.
portMUX_TYPE waiterMux = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

void ActivityManager::begin() {
  // 12 KB stack: SD card font rendering adds depth (file open + read + decode
  // + ring-buffer insert) on top of an already-deep reader render path, which
  // overflowed the previous 8 KB during on-demand glyph loading.
  // Pin the render task. On a dual-core part (ESP32-S3 / X4 Pro) putting long
  // renders and cover decodes on CPU 1 keeps them clear of CPU 0's idle watchdog;
  // on single-core (ESP32-C3 / X3 / X4) core 0 is the only option and this is
  // equivalent to the previous unpinned xTaskCreate.
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          12288,              // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore);
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    // isRenderable(): a session that has released the framebuffer (the web
    // server) keeps its last image on the panel and must not draw again.
    // Requests that arrive anyway -- USB plug/unplug, a maintenance repaint --
    // are dropped here rather than at each call site. The waiter notification
    // below still fires, so requestUpdateAndWait() cannot deadlock.
    if (currentActivity && renderer.isRenderable()) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Drop the previous paint's Full Touch action-bar slots (X4 Pro; a no-op
      // elsewhere). A screen that draws a hint bar republishes them from
      // drawButtonHints; one that doesn't must not inherit the last screen's
      // tap targets, which would still be live while nothing is drawn there.
      ActionBar::clear();
      // ...and tell it whether this screen even wants a Confirm slot. A menu
      // whose rows activate on a tap does not: there the label would just be a
      // second way to run what the row already runs. Published here rather than
      // at the drawButtonHints call sites because the answer is the activity's
      // and moves with its state (a modal opening puts the slot back).
      ActionBar::setConfirmRedundant(currentActivity->tapActivatesConfirm());
      currentActivity->render(std::move(lock));
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&waiterMux);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&waiterMux);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      // Before the incoming screen allocates anything, not after. The main loop
      // also enforces this policy, but only on its *next* iteration — by which
      // time onEnter() has already had to find its buffers with the BLE stack
      // still holding ~56KB.
      releaseBluetoothIfUnused();
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate) {
    requestedUpdate = false;
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToSettings() { replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput)); }

void ActivityManager::goToStats() { replaceActivity(std::make_unique<ReadingStatsActivity>(renderer, mappedInput)); }

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToTagBrowser() {
  replaceActivity(std::make_unique<GroupBrowserActivity>(renderer, mappedInput, GroupBrowserActivity::GroupMode::Tags));
}

void ActivityManager::goToAuthorBrowser() {
  replaceActivity(
      std::make_unique<GroupBrowserActivity>(renderer, mappedInput, GroupBrowserActivity::GroupMode::Authors));
}

void ActivityManager::goToSeriesBrowser() {
  replaceActivity(
      std::make_unique<GroupBrowserActivity>(renderer, mappedInput, GroupBrowserActivity::GroupMode::Series));
}

void ActivityManager::goToLibrary() { replaceActivity(std::make_unique<LibraryActivity>(renderer, mappedInput)); }

void ActivityManager::goToBrowser() {
  replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput));
}

void ActivityManager::goToReader(std::string path) {
  ensureSdFontLoaded();
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::goHome() { replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::isReaderActivity() const { return currentActivity && currentActivity->isReaderActivity(); }

bool ActivityManager::consumesTouchInput() const { return currentActivity && currentActivity->consumesTouchInput(); }

bool ActivityManager::handlesDirectTouch() const { return currentActivity && currentActivity->handlesDirectTouch(); }

bool ActivityManager::keepsBluetoothActive() const {
  return currentActivity && currentActivity->keepsBluetoothActive();
}

void ActivityManager::releaseBluetoothIfUnused() {
  if (!currentActivity || currentActivity->keepsBluetoothActive()) {
    return;
  }
  auto& btMgr = BluetoothHIDManager::getInstance();
  if (!btMgr.isEnabled()) {
    return;
  }
  LOG_INF("ACT", "Releasing Bluetooth before entering %s", currentActivity->name.c_str());
  btMgr.disable();
}

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&waiterMux);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&waiterMux);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock() { isLocked = (xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY) == pdTRUE); }

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  isLocked = (xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY) == pdTRUE);
}

RenderLock::~RenderLock() {
  if (isLocked) {
    if (xSemaphoreGetMutexHolder(activityManager.renderingMutex) == xTaskGetCurrentTaskHandle()) {
      xSemaphoreGive(activityManager.renderingMutex);
    } else {
      LOG_ERR("ACT", "RenderLock destructor owner mismatch; skipping give");
    }
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    if (xSemaphoreGetMutexHolder(activityManager.renderingMutex) == xTaskGetCurrentTaskHandle()) {
      xSemaphoreGive(activityManager.renderingMutex);
    } else {
      LOG_ERR("ACT", "RenderLock::unlock owner mismatch; skipping give");
    }
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
