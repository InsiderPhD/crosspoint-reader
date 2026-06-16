#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;

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

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = !SETTINGS.longPressChapterSkip;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool swapFront =
      SETTINGS.frontButtonFollowOrientation && (SETTINGS.orientation == CrossPointSettings::INVERTED ||
                                                SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW);
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const bool prev = tiltPrev || (usePress ? (input.wasPressed(MappedInputManager::Button::PageBack) ||
                                             input.wasPressed(prevButton))
                                          : (input.wasReleased(MappedInputManager::Button::PageBack) ||
                                             input.wasReleased(prevButton)));
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
