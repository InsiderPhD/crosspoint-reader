#include "SpinePageIndex.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

namespace {
constexpr const char* FILE_NAME = "/pages.bin";
}  // namespace

namespace SpinePageIndex {

bool read(const std::string& bookCachePath, const uint32_t layoutKey, const uint16_t spineCount,
          std::vector<uint16_t>& counts) {
  counts.clear();
  const std::string path = bookCachePath + FILE_NAME;
  if (!Storage.exists(path.c_str())) return false;
  FsFile f;
  if (!Storage.openFileForRead("SPI", path, f)) return false;
  uint32_t magic = 0, key = 0;
  uint8_t version = 0;
  uint16_t count = 0;
  const bool headerOk = serialization::tryReadPod(f, magic) && magic == MAGIC &&
                        serialization::tryReadPod(f, version) && version == VERSION &&
                        serialization::tryReadPod(f, key) && serialization::tryReadPod(f, count);
  if (!headerOk || key != layoutKey || count != spineCount) {
    f.close();
    return false;
  }
  counts.resize(count, 0);
  const size_t bytes = static_cast<size_t>(count) * sizeof(uint16_t);
  const bool ok = count == 0 || static_cast<size_t>(f.read(counts.data(), bytes)) == bytes;
  f.close();
  if (!ok) {
    counts.clear();
    return false;
  }
  return true;
}

bool writeAll(const std::string& bookCachePath, const uint32_t layoutKey, const std::vector<uint16_t>& counts) {
  const std::string path = bookCachePath + FILE_NAME;
  FsFile f;
  if (!Storage.openFileForWrite("SPI", path, f)) return false;
  serialization::writePod(f, MAGIC);
  serialization::writePod(f, VERSION);
  serialization::writePod(f, layoutKey);
  serialization::writePod(f, static_cast<uint16_t>(counts.size()));
  const size_t bytes = counts.size() * sizeof(uint16_t);
  const bool ok = counts.empty() || f.write(counts.data(), bytes) == bytes;
  f.close();
  if (!ok) {
    LOG_ERR("SPI", "pages.bin write failed");
    Storage.remove(path.c_str());
  }
  return ok;
}

bool record(const std::string& bookCachePath, const uint32_t layoutKey, const uint16_t spineCount,
            const uint16_t spineIndex, const uint16_t pages) {
  if (spineIndex >= spineCount) return false;
  std::vector<uint16_t> counts;
  if (!read(bookCachePath, layoutKey, spineCount, counts)) {
    counts.assign(spineCount, 0);
  }
  if (counts[spineIndex] == pages) return true;  // already recorded: no SD write
  counts[spineIndex] = pages;
  return writeAll(bookCachePath, layoutKey, counts);
}

}  // namespace SpinePageIndex
