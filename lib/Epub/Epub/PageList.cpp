#include "PageList.h"

#include <Logging.h>

#include <cstring>

namespace PageList {

namespace {

constexpr char FILE_NAME[] = "/pagelist.bin";

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Reads record `index` from an already open file.
bool readRecord(FsFile& file, const uint16_t index, Entry& out) {
  uint8_t raw[RECORD_SIZE];
  if (!file.seek(HEADER_SIZE + static_cast<size_t>(index) * RECORD_SIZE)) return false;
  if (file.read(raw, RECORD_SIZE) != static_cast<int>(RECORD_SIZE)) return false;
  memcpy(out.label, raw, LABEL_SIZE);
  out.label[LABEL_SIZE - 1] = '\0';
  out.spineIndex = static_cast<uint16_t>(raw[LABEL_SIZE] | (raw[LABEL_SIZE + 1] << 8));
  memcpy(out.anchor, raw + LABEL_SIZE + 2, ANCHOR_SIZE);
  out.anchor[ANCHOR_SIZE - 1] = '\0';
  return true;
}

// Opens the sidecar and returns its entry count (0 = unusable, file closed).
uint16_t openForRead(const std::string& cachePath, FsFile& file) {
  const std::string path = filePath(cachePath);
  // exists() first: openFileForRead logs an error for a missing file, and most
  // books have no page list.
  if (!Storage.exists(path.c_str())) return 0;
  if (!Storage.openFileForRead("PGL", path, file)) return 0;
  uint8_t header[HEADER_SIZE];
  if (file.read(header, HEADER_SIZE) != static_cast<int>(HEADER_SIZE) || header[0] != FILE_VERSION) {
    file.close();
    return 0;
  }
  const uint16_t total = static_cast<uint16_t>(header[2] | (header[3] << 8));
  if (total == 0) file.close();
  return total;
}

}  // namespace

std::string filePath(const std::string& cachePath) { return cachePath + FILE_NAME; }

void Writer::begin(const std::string& cachePath) {
  count = 0;
  path = filePath(cachePath);
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  open = Storage.openFileForWrite("PGL", path, file);
  if (!open) return;
  const uint8_t header[HEADER_SIZE] = {FILE_VERSION, 0, 0, 0};  // count patched in finish()
  file.write(header, HEADER_SIZE);
}

void Writer::add(const std::string& label, const int spineIndex, const std::string& anchor) {
  if (!open || count >= MAX_ENTRIES) return;
  if (spineIndex < 0 || spineIndex > UINT16_MAX || anchor.size() >= ANCHOR_SIZE) return;

  // Labels arrive with the markup's whitespace around them.
  size_t begin = 0;
  size_t end = label.size();
  while (begin < end && isSpace(label[begin])) ++begin;
  while (end > begin && isSpace(label[end - 1])) --end;
  if (begin == end) return;
  size_t labelLen = end - begin;
  if (labelLen >= LABEL_SIZE) {
    // Never cut a UTF-8 sequence in half.
    labelLen = LABEL_SIZE - 1;
    while (labelLen > 0 && (static_cast<uint8_t>(label[begin + labelLen]) & 0xC0) == 0x80) --labelLen;
  }

  uint8_t raw[RECORD_SIZE] = {};
  memcpy(raw, label.data() + begin, labelLen);
  raw[LABEL_SIZE] = static_cast<uint8_t>(spineIndex & 0xFF);
  raw[LABEL_SIZE + 1] = static_cast<uint8_t>((spineIndex >> 8) & 0xFF);
  memcpy(raw + LABEL_SIZE + 2, anchor.data(), anchor.size());
  if (file.write(raw, RECORD_SIZE) != RECORD_SIZE) {
    LOG_ERR("PGL", "Short write; page list abandoned");
    file.close();
    open = false;
    count = 0;
    Storage.remove(path.c_str());
    return;
  }
  ++count;
}

uint16_t Writer::finish() {
  if (open) {
    if (count > 0) {
      const uint8_t countBytes[2] = {static_cast<uint8_t>(count & 0xFF), static_cast<uint8_t>(count >> 8)};
      file.seek(2);
      file.write(countBytes, sizeof(countBytes));
    }
    file.close();
    open = false;
    if (count == 0) Storage.remove(path.c_str());
  }
  // Give the path's heap back: the Writer lives inside BookMetadataCache.
  std::string().swap(path);
  std::string().swap(memoHref);
  memoValid = false;
  const uint16_t written = count;
  count = 0;
  if (written > 0) LOG_DBG("PGL", "Wrote %u page-list entries", written);
  return written;
}

uint16_t count(const std::string& cachePath) {
  FsFile file;
  const uint16_t total = openForRead(cachePath, file);
  if (total > 0) file.close();
  return total;
}

bool read(const std::string& cachePath, const uint16_t index, Entry& out) {
  FsFile file;
  const uint16_t total = openForRead(cachePath, file);
  if (total == 0) return false;
  const bool ok = index < total && readRecord(file, index, out);
  file.close();
  return ok;
}

uint16_t lowerBoundSpine(const std::string& cachePath, const uint16_t total, const uint16_t spineIndex) {
  FsFile file;
  const uint16_t fileTotal = openForRead(cachePath, file);
  if (fileTotal == 0) return total;
  uint16_t lo = 0;
  uint16_t hi = total < fileTotal ? total : fileTotal;
  Entry entry;
  while (lo < hi) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
    if (!readRecord(file, mid, entry)) break;
    if (entry.spineIndex < spineIndex) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      hi = mid;
    }
  }
  file.close();
  return lo;
}

}  // namespace PageList
