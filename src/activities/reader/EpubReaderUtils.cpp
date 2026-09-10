#include "EpubReaderUtils.h"

#include <HalStorage.h>
#include <Logging.h>

#include "RecentBooksStore.h"

namespace EpubReaderUtils {

bool saveProgress(Epub& epub, const int spineIndex, const int currentPage, const int pageCount) {
  bool written = false;

  FsFile f;
  if (Storage.openFileForWrite("ERS", epub.getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = spineIndex & 0xFF;
    data[1] = (spineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    written = true;
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", spineIndex, currentPage);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }

  const float chapterProgress =
      (pageCount > 0) ? static_cast<float>(currentPage) / static_cast<float>(pageCount) : 0.0f;
  RECENT_BOOKS.updateProgress(epub.getPath(),
                              static_cast<int8_t>(epub.calculateProgress(spineIndex, chapterProgress) * 100.0f));
  return written;
}

}  // namespace EpubReaderUtils
