#include "BookSettings.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace BookSettings {

namespace {

constexpr uint8_t FILE_VERSION = 1;
constexpr char FILE_NAME[] = "/book_settings.bin";
constexpr uint32_t FONT_BIT = 1u << 0;
constexpr uint32_t ALL_BITS = (1u << kFieldCount) - 1;
constexpr size_t SD_FONT_LEN = sizeof(CrossPointSettings::sdFontFamilyName);
// "/.crosspoint/epub_" + a 20-digit hash is 38 characters.
constexpr size_t CACHE_DIR_LEN = 48;
// Value bytes a file may carry; more than this build knows is fine (see load()).
constexpr size_t MAX_FILE_FIELDS = 32;

static_assert(kFieldCount <= MAX_FILE_FIELDS, "override mask is a uint32_t");

struct Values {
  uint8_t v[kFieldCount];
  char sdFont[SD_FONT_LEN];
};

// The whole resident cost of the feature: fixed, in .bss, ~150 bytes.
struct State {
  bool active = false;
  bool suspended = false;
  uint32_t mask = 0;
  Values global{};  // global value of every overridden field (valid where mask is set)
  Values book{};    // the book's values, parked while suspended
  char cacheDir[CACHE_DIR_LEN] = "";
};
State st;

int indexOf(uint8_t CrossPointSettings::* member) {
  for (size_t i = 0; i < kFieldCount; ++i) {
    if (kFields[i].member == member) return static_cast<int>(i);
  }
  return -1;
}

void copySdFont(char* dst, const char* src) { snprintf(dst, SD_FONT_LEN, "%s", src); }

bool filePath(char* out, const size_t outSize) {
  const int n = snprintf(out, outSize, "%s%s", st.cacheDir, FILE_NAME);
  return n > 0 && static_cast<size_t>(n) < outSize;
}

// Copies the fields selected by mask between SETTINGS and a Values block.
void capture(Values& out, const uint32_t mask) {
  for (size_t i = 0; i < kFieldCount; ++i) {
    if (mask & (1u << i)) out.v[i] = SETTINGS.*(kFields[i].member);
  }
  if (mask & FONT_BIT) copySdFont(out.sdFont, SETTINGS.sdFontFamilyName);
}

void apply(const Values& in, const uint32_t mask) {
  for (size_t i = 0; i < kFieldCount; ++i) {
    if (mask & (1u << i)) SETTINGS.*(kFields[i].member) = in.v[i];
  }
  if (mask & FONT_BIT) copySdFont(SETTINGS.sdFontFamilyName, in.sdFont);
}

// Reads the sidecar into (mask, values). A missing file is the normal case.
bool load(uint32_t& mask, Values& values) {
  mask = 0;
  char path[CACHE_DIR_LEN + sizeof(FILE_NAME)];
  if (!filePath(path, sizeof(path)) || !Storage.exists(path)) return false;

  FsFile file;
  if (!Storage.openFileForRead("BKS", path, file)) return false;

  uint8_t header[6];  // version, field count, mask (little-endian)
  bool ok = file.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) && header[0] == FILE_VERSION;
  const uint8_t fileFields = ok ? header[1] : 0;
  uint8_t raw[MAX_FILE_FIELDS] = {};
  ok = ok && fileFields <= MAX_FILE_FIELDS && file.read(raw, fileFields) == static_cast<int>(fileFields) &&
       file.read(values.sdFont, SD_FONT_LEN) == static_cast<int>(SD_FONT_LEN);
  file.close();
  if (!ok) {
    LOG_ERR("BKS", "Ignoring unreadable %s", path);
    return false;
  }
  values.sdFont[SD_FONT_LEN - 1] = '\0';

  uint32_t fileMask;
  memcpy(&fileMask, header + 2, sizeof(fileMask));
  // A file written by a newer firmware may carry fields this build does not
  // know; a damaged one may carry values out of range. Drop those overrides
  // rather than the whole file.
  for (size_t i = 0; i < kFieldCount && i < fileFields; ++i) {
    if (!(fileMask & (1u << i))) continue;
    if (raw[i] < kFields[i].min || raw[i] > kFields[i].max) continue;
    values.v[i] = raw[i];
    mask |= 1u << i;
  }
  return mask != 0;
}

}  // namespace

void begin(const std::string& cacheDir) {
  end();
  if (cacheDir.empty() || cacheDir.size() >= CACHE_DIR_LEN) {
    LOG_ERR("BKS", "Cache path unusable for book settings (%u chars)", static_cast<unsigned>(cacheDir.size()));
    return;
  }
  memcpy(st.cacheDir, cacheDir.c_str(), cacheDir.size() + 1);
  st.active = true;

  uint32_t mask = 0;
  if (load(mask, st.book)) {
    capture(st.global, mask);
    apply(st.book, mask);
    st.mask = mask;
    LOG_INF("BKS", "Book overrides reader settings (mask 0x%lx)", static_cast<unsigned long>(mask));
  }
}

void end() {
  if (!st.active) return;
  if (!st.suspended) apply(st.global, st.mask);
  st = State{};
}

bool isActive() { return st.active; }

bool hasOverrides() { return st.active && st.mask != 0; }

bool isOverridden(uint8_t CrossPointSettings::* member) {
  const int i = indexOf(member);
  return st.active && i >= 0 && (st.mask & (1u << i)) != 0;
}

bool beginOverride(uint8_t CrossPointSettings::* member) {
  const int i = indexOf(member);
  if (!st.active || st.suspended || i < 0) return false;
  const uint32_t bit = 1u << i;
  // Not yet overridden, so the live value IS the global one: remember it.
  if (!(st.mask & bit)) {
    capture(st.global, bit);
    st.mask |= bit;
  }
  return true;
}

bool save() {
  if (!st.active || st.suspended) return false;
  char path[CACHE_DIR_LEN + sizeof(FILE_NAME)];
  if (!filePath(path, sizeof(path))) return false;

  if (st.mask == 0) {
    if (Storage.exists(path)) Storage.remove(path);
    return true;
  }

  Values live{};
  capture(live, ALL_BITS);
  uint8_t header[6] = {FILE_VERSION, static_cast<uint8_t>(kFieldCount)};
  memcpy(header + 2, &st.mask, sizeof(st.mask));

  Storage.mkdir(st.cacheDir);
  FsFile file;
  if (!Storage.openFileForWrite("BKS", path, file)) {
    LOG_ERR("BKS", "Could not open %s for write", path);
    return false;
  }
  const bool ok = file.write(header, sizeof(header)) == sizeof(header) &&
                  file.write(live.v, kFieldCount) == kFieldCount && file.write(live.sdFont, SD_FONT_LEN) == SD_FONT_LEN;
  file.close();
  if (!ok) {
    LOG_ERR("BKS", "Short write to %s", path);
    Storage.remove(path);
  }
  return ok;
}

bool saveIfOverridden(uint8_t CrossPointSettings::* member) {
  if (st.suspended || !isOverridden(member)) return false;
  save();
  return true;
}

void reset() {
  if (!st.active || st.suspended) return;
  apply(st.global, st.mask);
  st.mask = 0;
  save();  // no overrides left: removes the sidecar
}

void suspend() {
  if (!st.active || st.suspended) return;
  capture(st.book, st.mask);
  apply(st.global, st.mask);
  st.suspended = true;
}

void resume() {
  if (!st.active || !st.suspended) return;
  st.suspended = false;
  capture(st.global, st.mask);  // whatever the global editor left behind
  apply(st.book, st.mask);
}

uint8_t globalValue(const CrossPointSettings& s, uint8_t CrossPointSettings::* member) {
  if (st.active && !st.suspended && st.mask != 0 && &s == &SETTINGS) {
    const int i = indexOf(member);
    if (i >= 0 && (st.mask & (1u << i))) return st.global.v[i];
  }
  return s.*member;
}

const char* globalString(const CrossPointSettings& s, const char* field) {
  if (st.active && !st.suspended && (st.mask & FONT_BIT) && field == s.sdFontFamilyName && &s == &SETTINGS) {
    return st.global.sdFont;
  }
  return field;
}

}  // namespace BookSettings
