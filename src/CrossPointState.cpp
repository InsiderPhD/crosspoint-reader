#include "CrossPointState.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>
#include <esp_rom_crc.h>

#include <algorithm>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 4;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

uint8_t CrossPointState::recentSleepAge(uint16_t idx) const {
  const uint8_t fill = std::min(recentSleepFill, SLEEP_RECENT_COUNT);
  for (uint8_t i = 0; i < fill; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return i;
  }
  return SLEEP_NOT_RECENT;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

void CrossPointState::clearRecentSleepHistory() {
  std::fill_n(recentSleepImages, SLEEP_RECENT_COUNT, static_cast<uint16_t>(0));
  recentSleepPos = 0;
  recentSleepFill = 0;
}

void CrossPointState::registerValidTimeSync(const uint32_t validTimestamp) {
  if (validTimestamp > 0) {
    lastKnownValidTimestamp = validTimestamp;
  }
}

bool CrossPointState::saveToFile() const {
  // Serialise first and fingerprint the exact bytes that would be written. The
  // String is the one a save always built; the CRC adds no allocation.
  String json;
  JsonSettingsIO::serializeState(*this, json);
  const uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(json.c_str()), json.length());

  // Skip the card write when the file already holds these bytes. saveToFile()
  // runs on every sleep entry, every wake and every reader open, and most of
  // those change nothing; each skipped call saves a FAT directory update and a
  // data-sector rewrite. exists() keeps the skip honest if the file was removed
  // behind our back (cache clear, file transfer).
  if (lastSavedCrcValid && crc == lastSavedCrc && Storage.exists(STATE_FILE_JSON)) {
    return true;
  }

  // A second task saving while this write is in flight (the boot NTP task) may
  // land after it; the generation check then drops the remembered CRC rather
  // than trusting one that no longer describes the file.
  const uint32_t generation = ++saveGeneration;
  lastSavedCrcValid = false;
  Storage.mkdir("/.crosspoint");
  if (!Storage.writeFile(STATE_FILE_JSON, json)) {
    return false;
  }
  if (saveGeneration == generation) {
    lastSavedCrc = crc;
    lastSavedCrcValid = true;
  }
  return true;
}

bool CrossPointState::loadFromFile() {
  // A reload replaces the in-memory state; forget the remembered snapshot.
  lastSavedCrcValid = false;
  // Try JSON first
  if (Storage.exists(STATE_FILE_JSON)) {
    String json = Storage.readFile(STATE_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadState(*this, json.c_str());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(STATE_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
  }

  return false;
}

bool CrossPointState::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  return true;
}
