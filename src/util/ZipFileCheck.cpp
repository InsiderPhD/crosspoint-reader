#include "ZipFileCheck.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <cstring>

namespace ZipFileCheck {

bool looksComplete(const char* path) {
  FsFile f = Storage.open(path, O_RDONLY);
  if (!f) {
    LOG_ERR("ZIPCHK", "Cannot open %s", path);
    return false;
  }
  const size_t size = f.fileSize();
  if (size < 22) {
    LOG_ERR("ZIPCHK", "%s: %u bytes is too small for a ZIP", path, (unsigned)size);
    f.close();
    return false;
  }

  uint8_t buf[256];
  if (!f.seek(0) || f.read(buf, 4) != 4 || memcmp(buf, "PK\x03\x04", 4) != 0) {
    LOG_ERR("ZIPCHK", "%s: no local file header at offset 0", path);
    f.close();
    return false;
  }

  // Walk backwards over the tail in overlapping windows (3-byte overlap so a
  // signature straddling two windows is still seen). EOCD = 50 4b 05 06.
  const size_t maxScan = size < 22u + 65535u ? size : 22u + 65535u;
  size_t end = size;  // exclusive
  size_t scanned = 0;
  bool found = false;
  while (!found && scanned < maxScan && end >= 4) {
    size_t take = sizeof(buf);
    if (take > end) take = end;
    if (take > maxScan - scanned + 3) take = maxScan - scanned + 3;
    const size_t start = end - take;
    if (!f.seek(start) || f.read(buf, take) != static_cast<int>(take)) {
      LOG_ERR("ZIPCHK", "%s: read failed at %u", path, (unsigned)start);
      break;
    }
    for (size_t i = take - 4;; --i) {
      if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
        found = true;
        break;
      }
      if (i == 0) break;
    }
    if (take <= 3) break;
    end = start + 3;
    scanned += take - 3;
  }
  f.close();
  if (!found) {
    LOG_ERR("ZIPCHK", "%s: no end-of-central-directory record in the last %u bytes", path, (unsigned)maxScan);
  }
  return found;
}

}  // namespace ZipFileCheck
