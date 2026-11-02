#include "SettingsList.h"

// Out of line so the ~100 entry constructors are emitted once, in this file,
// rather than inlined into every caller of forEachSetting().
void forEachSetting(const SettingsListDetail::SettingSink::Visit visit, void* context) {
  SettingsListDetail::SettingSink sink(visit, context);
  SettingsListDetail::appendDisplaySettings(sink);
  SettingsListDetail::appendReaderTypographySettings(sink);
  SettingsListDetail::appendReaderBehaviourSettings(sink);
  SettingsListDetail::appendStatsSettings(sink);
  SettingsListDetail::appendControlSettings(sink);
  SettingsListDetail::appendSystemSettings(sink);
  SettingsListDetail::appendKoReaderSettings(sink);
  SettingsListDetail::appendHardcoverSettings(sink);
  SettingsListDetail::appendOpdsSettings(sink);
  SettingsListDetail::appendStatusBarSettings(sink);
  constexpr size_t expectedCount =
      SettingsListDetail::SETTINGS_LIST_UNCONDITIONAL_COUNT + SettingsListDetail::SETTINGS_LIST_FRONTLIGHT_COUNT;
  if (sink.size() != expectedCount) {
    LOG_ERR("SET", "Settings list emitted %u entries, expected %u: update SETTINGS_LIST_*_COUNT",
            static_cast<unsigned>(sink.size()), static_cast<unsigned>(expectedCount));
  }
}
