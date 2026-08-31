#pragma once

#include <functional>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, BOOKFUSION, LIBBY };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "BookFusion" - Browse a linked BookFusion library (only once an account is linked)
 * - "Libby" - Borrow library loans; joins a network and serves the /libby page
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;

  // Confirm the highlighted mode — the Confirm press body, also fired by a
  // Full Touch tap on the selected row.
  void handleSelection();

  // Selecting Libby raises a note pointing at the web UI rather than entering a
  // mode -- there is no on-device Libby flow any more. Any key clears it.
  bool showingLibbyNote = false;
  Rect listRect() const;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NetworkModeSelection", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // While the Libby note is up the screen wants any contact at all to dismiss
  // it, so it hands touch back to the global tap-is-Confirm injection rather
  // than hit-testing rows behind the popup.
  bool handlesDirectTouch() const override { return !showingLibbyNote; }

  void onModeSelected(NetworkMode mode);
  void onCancel();
};
