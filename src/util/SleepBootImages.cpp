#include "SleepBootImages.h"

#include <Logging.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"

namespace SleepBootImages {

void setBootImage(const std::string& path) {
  if (APP_STATE.bootImagePath == path) return;

  APP_STATE.bootImagePath = path;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("IMG", "Failed to save boot image: %s", path.c_str());
  }
  LOG_INF("IMG", "Boot image: %s", path.empty() ? "<built-in>" : path.c_str());
}

void setSleepFolder(const std::string& path) {
  if (APP_STATE.preferredSleepFolderPath == path) return;

  APP_STATE.preferredSleepFolderPath = path;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("IMG", "Failed to save sleep folder: %s", path.c_str());
  }
  LOG_INF("IMG", "Sleep folder: %s", path.empty() ? "<default>" : path.c_str());

  // Only the custom-image sleep screens read the folder. Choosing one is a
  // request to see its images, so switch a mode that would ignore them.
  if (!path.empty() && SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM &&
      SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
  }
}

}  // namespace SleepBootImages
