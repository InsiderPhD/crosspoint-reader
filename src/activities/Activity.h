#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }
  // True for activities that read raw touch events themselves (tap-zone and
  // home-key dispatch in the readers, tap hit-testing in the keyboard). The
  // main loop then disables the global tap-anywhere-is-Confirm and
  // home-key-is-Confirm injections (X4 Pro) so a consumed touch cannot also
  // fire Confirm. Deliberately NOT tied to isReaderActivity(): reader
  // sub-screens (menu, chapter select, footnotes) claim reader status for the
  // Bluetooth lifecycle but are plain menus that want the Confirm injections.
  virtual bool consumesTouchInput() const { return false; }
  // True for activities that hit-test taps against their own drawn UI when
  // Full Touch mode (SETTINGS.fullTouchUi, X4 Pro) is enabled. The main loop
  // then disables ONLY the tap-anywhere-is-Confirm injection; the
  // home-key-tap-is-Confirm injection stays on (these screens have no home-key
  // geometry of their own). Deliberately separate from consumesTouchInput(),
  // which kills both. May be dynamic: return false while a modal without tap
  // hit-testing is open, so tap-activates-the-highlighted-option comes back
  // for the modal.
  virtual bool handlesDirectTouch() const { return false; }
  // X4 Pro Full Touch only. True while a tap on the highlighted element does
  // exactly what the action bar's Confirm slot would do, which makes that slot
  // a second way to run the same handler. ActionBar then leaves it out and the
  // remaining slots take its width, so a plain menu shows one wide "Back"
  // rather than "Back | Select" -- the rows themselves are the Select.
  //
  // Defaults to handlesDirectTouch() because that is precisely what those
  // screens do: the first tap moves the cursor to a row, a second tap on it
  // calls the same function the Confirm branch calls (TouchListNav::tapRow's
  // Activated case, or the hand-rolled equivalent).
  //
  // Override to false on a screen whose Confirm does something its drawn UI
  // cannot: the date/number spinners are that case here -- a second tap steps
  // the field's value and only Confirm commits it. Get this wrong and the user
  // is stranded: this board has no front buttons, and handlesDirectTouch() has
  // already switched off the tap-anywhere-is-Confirm injection, so the bar slot
  // is the only remaining way to reach Confirm.
  virtual bool tapActivatesConfirm() const { return handlesDirectTouch(); }
  // Bluetooth (BLE page-turner) is only allowed to stay up in activities that
  // actually use it — the reader and the Bluetooth settings screen. Everywhere
  // else the main loop shuts the stack down to return its heap.
  virtual bool keepsBluetoothActive() const { return isReaderActivity(); }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome();
  void onSelectBook(const std::string& path);
};
