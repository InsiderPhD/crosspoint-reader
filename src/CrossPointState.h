#pragma once
#include <cstdint>
#include <string>

// Battery level against awake-on-battery time for the current charge cycle
// (see util/PowerHistory). Fixed size and part of this singleton, so tracking
// costs no heap; it rides the state.json save that deep sleep already makes.
struct PowerHistoryState {
  static constexpr uint8_t SAMPLE_CAPACITY = 21;  // 100% down to 0% in 5% steps

  uint32_t activeSeconds = 0;
  uint32_t cycleStartDay = 0;  // local day ordinal, valid when hasCycleStartDay
  uint32_t readerPageTurns = 0;
  uint16_t sampleActiveMinutes[SAMPLE_CAPACITY] = {};
  uint8_t samplePercents[SAMPLE_CAPACITY] = {};
  uint8_t sampleCount = 0;
  uint8_t lastPercent = UINT8_MAX;  // > 100 = never sampled
  // Lowest level seen while on external power, so a later rise reads as a charge.
  uint8_t chargePendingPercent = UINT8_MAX;
  bool hasCycleStartDay = false;
  bool cycleConfirmed = false;  // the cycle began at an actual charge
  bool chargePending = false;
};

static_assert(sizeof(PowerHistoryState) <= 96, "Power history must remain a small fixed-size state");

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  // Folder the Custom sleep screen picks its images from, chosen in the file
  // browser. Empty (the default) keeps the built-in lookup: /sleep.bmp, then
  // /.sleep, then /sleep. A set folder that has since gone missing falls back
  // to that same lookup.
  std::string preferredSleepFolderPath;
  // Which folder the recentSleepImages indices below were counted in (a
  // non-zero hash of its path). The history holds positions in a directory
  // listing, so it only means something for one folder: the picker drops it
  // when it runs against a different one. 0 = not recorded yet (state.json
  // from an older firmware), adopted without dropping anything.
  uint16_t recentSleepFolderHash = 0;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  // Reader's last computed "time left in book" (pages-left × reading pace),
  // stashed at reader exit so the sleep screen can show it without re-deriving
  // a pace from whole-percent progress (which hides after short sessions).
  // Persisted in state.json: the sleep screen can repaint in a boot where the
  // reader never ran (wake to home -> timeout, silent/crash reboot), and the
  // estimate only changes inside the reader so a persisted value stays in step
  // with progress. bookPath tags which book it was computed for.
  std::string readerTimeLeftBookPath;
  uint32_t readerTimeLeftSeconds = 0;
  uint32_t lastKnownValidTimestamp = 0;
  // Local day ordinal of the last whole-library Hardcover push, so the boot
  // sync runs at most once a day. 0 = never. Persisted in state.json because
  // the whole point is to survive the reboot.
  uint32_t lastHardcoverSyncDay = 0;
  bool showBootScreen = true;
  // BMP shown in place of the built-in boot logo, chosen in the file browser.
  // Empty (the default) = built-in logo, and the boot path never touches the
  // card for it. A path that no longer opens or parses also gives the logo.
  std::string bootImagePath;
  PowerHistoryState powerHistory;

  // recentSleepAge() result for an index that is not in the history.
  static constexpr uint8_t SLEEP_NOT_RECENT = UINT8_MAX;

  // How many picks ago idx was last shown: 0 = the most recent pick. Returns
  // SLEEP_NOT_RECENT when idx is not in the history.
  uint8_t recentSleepAge(uint16_t idx) const;

  // One-shot "open this EPUB at this spot" request, set when a clipping is opened from outside
  // the reader (Stats > Clippings, Book Info). Memory only, never persisted; consumed by
  // EpubReaderActivity::onEnter when the path matches, so it can't leak into a later open.
  struct PendingOpen {
    std::string path;
    uint16_t spineIndex = 0;
    uint16_t page = 0;
    uint16_t pageCount = 0;
  } pendingOpen;

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  void clearRecentSleepHistory();

  // Persist a freshly-validated wall-clock timestamp so future boots can fall back to it
  // for header date display when NTP isn't available this boot. Caller must still saveToFile().
  void registerValidTimeSync(uint32_t validTimestamp);

  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
  // CRC32 of the JSON last written successfully, so saveToFile() can skip a
  // card write that would change nothing. Valid only while lastSavedCrcValid.
  mutable uint32_t lastSavedCrc = 0;
  mutable uint32_t saveGeneration = 0;
  mutable bool lastSavedCrcValid = false;
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
