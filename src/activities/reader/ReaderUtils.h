#pragma once

#include <BluetoothHIDManager.h>
#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr int FOOTNOTE_HINT_HEIGHT = 20;

// A section build's peak demand is one 32KB CONTIGUOUS malloc for the inflate
// ring buffer (InflateReader::init), plus working room for the parser and file
// buffers. Free bytes are never the constraint here — the largest contiguous
// block is. Measured on hardware: 96KB free with a 27KB largest block fails.
constexpr size_t SECTION_BUILD_INFLATE_BYTES = 32768;
constexpr size_t SECTION_BUILD_HEADROOM_BYTES = 8192;
constexpr size_t SECTION_BUILD_REQUIRED_BLOCK = SECTION_BUILD_INFLATE_BYTES + SECTION_BUILD_HEADROOM_BYTES;

inline size_t largestFreeBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT); }

/** True when a section build would fit right now without freeing anything. */
inline bool sectionBuildFitsNow() { return largestFreeBlock() >= SECTION_BUILD_REQUIRED_BLOCK; }

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

/**
 * The footnote display mode a section build/load should actually use.
 *
 * Single point of truth on purpose. footnoteDisplay is a section-cache key
 * (Section.cpp compares it on load and stamps the *requested* value into the
 * header), so the build, the load and the render-time checks must all derive it
 * the same way. Disagreement means either a layout that expects footnotes the
 * build omitted, or a chapter cached as "footnotes on" that contains none and
 * never invalidates.
 *
 * This briefly excluded on-page footnotes whenever a BLE remote was paired, back
 * when the footnote table was a single ~35KB contiguous allocation (32 entries
 * each carrying an inline char[1024]) — bigger than the inflate dictionary, and
 * the allocation that aborted builds. That table is now a packed pool with a
 * 12KB largest block, comfortably under the 32KB inflate buffer that dominates a
 * build, so the exclusion no longer earned its cost and was removed.
 */
inline uint8_t effectiveFootnoteDisplay() { return SETTINGS.footnoteDisplay; }

/** True when footnotes should be laid out on the page for this build/render. */
inline bool footnotesOnPage() { return effectiveFootnoteDisplay() == CrossPointSettings::FOOTNOTE_ON_PAGE; }

inline bool allowLongPressChapterSkip() {
  // BLE remote presses are injected as pulses, so their hold duration is always
  // meaningless. Keep chapter-skip semantics for the local hardware buttons only.
  return SETTINGS.longPressChapterSkip && !BluetoothHIDManager::getInstance().hadRecentRemoteInput();
}

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = !allowLongPressChapterSkip();
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool swapFront =
      SETTINGS.frontButtonFollowOrientation && (SETTINGS.orientation == CrossPointSettings::INVERTED ||
                                                SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW);
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const bool prev =
      tiltPrev ||
      (usePress ? (input.wasPressed(MappedInputManager::Button::PageBack) || input.wasPressed(prevButton))
                : (input.wasReleased(MappedInputManager::Button::PageBack) || input.wasReleased(prevButton)));
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);
  const bool next = tiltNext || (usePress ? (input.wasPressed(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasPressed(nextButton))
                                          : (input.wasReleased(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasReleased(nextButton)));
  return {prev, next, tiltPrev || tiltNext};
}

// If the user has Power short-press mapped to FORCE_REFRESH and Power was just released,
// half-refresh the e-ink screen. Used by the reader activities; the global main loop no
// longer handles Power so the setting's scope stays reader-only (Power in list activities
// opens the sort menu).
inline bool detectAndApplyForceRefresh(const MappedInputManager& input, const GfxRenderer& renderer) {
  if (SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH) return false;
  if (!input.wasReleased(MappedInputManager::Button::Power)) return false;
  LOG_DBG("READER", "Manual screen refresh triggered");
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  return true;
}

// Called on each user-initiated page turn to update the rolling reading speed estimate.
// lastTurnMs should be a per-activity member initialised to 0.
inline void updateReadingSpeed(unsigned long& lastTurnMs) {
  const unsigned long now = millis();
  if (lastTurnMs > 0) {
    const unsigned long elapsed = now - lastTurnMs;
    // Only count if between 2s and 5 minutes — ignores idle gaps and accidental taps
    if (elapsed >= 2000UL && elapsed <= 300000UL) {
      const uint16_t secsPerPage = static_cast<uint16_t>(elapsed / 1000UL);
      if (SETTINGS.readingSpeedSecondsPerPage == 0) {
        SETTINGS.readingSpeedSecondsPerPage = secsPerPage;
      } else {
        // Exponential moving average, alpha = 0.15
        SETTINGS.readingSpeedSecondsPerPage = static_cast<uint16_t>(
            0.85f * static_cast<float>(SETTINGS.readingSpeedSecondsPerPage) + 0.15f * static_cast<float>(secsPerPage));
      }
    }
  }
  lastTurnMs = now;
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

}  // namespace ReaderUtils

// Shared by all reader activities: saves APP_STATE and enters deep sleep.
// Defined in EpubReaderActivity.cpp; declared here so other readers can call it.
void enterDeepSleepFromReaderAction();
