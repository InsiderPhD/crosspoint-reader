#include "BookFusionCoverCache.h"

#include <Bitmap.h>
#include <Epub.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include <cstdint>
#include <cstring>

#include "network/HttpDownloader.h"

namespace {

enum class CoverImageType { Unknown, Jpeg, Png };

std::string normalizeBookFusionCoverUrl(const char* coverUrl) {
  if (!coverUrl || coverUrl[0] == '\0') return {};

  std::string url = coverUrl;
  if (url.rfind("//", 0) == 0) {
    return "https:" + url;
  }
  if (url.rfind("/", 0) == 0) {
    return "https://www.bookfusion.com" + url;
  }
  return url;
}

CoverImageType detectCoverImageType(FsFile& file) {
  uint8_t header[8] = {};
  file.seek(0);
  const int read = file.read(header, sizeof(header));
  file.seek(0);

  if (read >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
    return CoverImageType::Jpeg;
  }
  if (read >= 8 && header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G' && header[4] == 0x0D &&
      header[5] == 0x0A && header[6] == 0x1A && header[7] == 0x0A) {
    return CoverImageType::Png;
  }
  return CoverImageType::Unknown;
}

bool validateCoverBmp(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("BFC", path, file)) return false;

  Bitmap bitmap(file);
  const auto err = bitmap.parseHeaders();
  file.close();

  if (err != BmpReaderError::Ok) {
    LOG_ERR("BFC", "BookFusion cover BMP validation failed (err=%d): %s", static_cast<int>(err), path.c_str());
    return false;
  }
  return true;
}

bool convertBookFusionCoverImage(const std::string& srcPath, const std::string& destPath, bool thumbnail,
                                 int thumbTargetWidth, int thumbTargetHeight, bool crop) {
  FsFile src;
  if (!Storage.openFileForRead("BFC", srcPath, src)) return false;

  const CoverImageType type = detectCoverImageType(src);
  if (type == CoverImageType::Unknown) {
    LOG_ERR("BFC", "BookFusion cover has unsupported image signature: %s", srcPath.c_str());
    src.close();
    return false;
  }

  FsFile dest;
  if (!Storage.openFileForWrite("BFC", destPath, dest)) {
    src.close();
    return false;
  }

  bool success = false;
  if (thumbnail) {
    // Grayscale (not 1-bit) thumbnails. The 1-bit converter is less robust and
    // fails on some cover JPEGs, leaving thumb_<H>.bmp missing even though the
    // grayscale cover.bmp from the same source succeeds — which is why some
    // downloaded books showed a cover and others didn't. Epub::generateThumbBmp
    // uses grayscale for exactly this reason ("avoid reading issues", Epub.cpp);
    // matching it makes BookFusion thumbnails render reliably in the home cards
    // and the library grid.
    if (type == CoverImageType::Jpeg) {
      success = JpegToBmpConverter::jpegFileToBmpStreamWithSize(src, dest, thumbTargetWidth, thumbTargetHeight);
    } else {
      success = PngToBmpConverter::pngFileToBmpStreamWithSize(src, dest, thumbTargetWidth, thumbTargetHeight);
    }
  } else {
    if (type == CoverImageType::Jpeg) {
      success = JpegToBmpConverter::jpegFileToBmpStream(src, dest, crop);
    } else {
      success = PngToBmpConverter::pngFileToBmpStream(src, dest, crop);
    }
  }

  src.close();
  dest.flush();
  dest.close();

  if (!success || !validateCoverBmp(destPath)) {
    LOG_ERR("BFC", "Failed to convert BookFusion cover BMP: %s", destPath.c_str());
    Storage.remove(destPath.c_str());
    return false;
  }

  return true;
}

}  // namespace

namespace BookFusionCoverCache {

bool refresh(const std::string& coverUrlRaw, const Epub& epub, int coverHeight, char* outThumbPath,
             size_t outThumbPathLen) {
  const std::string coverUrl = normalizeBookFusionCoverUrl(coverUrlRaw.c_str());
  if (coverUrl.empty()) {
    LOG_DBG("BFC", "No BookFusion cover URL to refresh for %s", epub.getCachePath().c_str());
    return false;
  }

  const std::string tempCoverPath = epub.getCachePath() + "/.bookfusion-cover";
  LOG_DBG("BFC", "Downloading BookFusion API cover into %s", epub.getCachePath().c_str());
  const auto downloadResult = HttpDownloader::downloadToFile(coverUrl, tempCoverPath, nullptr, false);
  if (downloadResult != HttpDownloader::OK) {
    LOG_ERR("BFC", "Failed to download BookFusion API cover into %s", epub.getCachePath().c_str());
    return false;
  }

  const int thumbTargetWidth = coverHeight * 0.6f;
  const int thumbTargetHeight = coverHeight;
  const std::string thumbPath = epub.getThumbBmpPath(coverHeight);
  const bool thumbOk =
      convertBookFusionCoverImage(tempCoverPath, thumbPath, true, thumbTargetWidth, thumbTargetHeight, true);

  // Prime both sleep-screen variants while WiFi is already on. SleepActivity
  // will later pick the one matching the user's cover mode.
  const bool fitOk = convertBookFusionCoverImage(tempCoverPath, epub.getCoverBmpPath(false), false, 0, 0, false);
  const bool cropOk = convertBookFusionCoverImage(tempCoverPath, epub.getCoverBmpPath(true), false, 0, 0, true);

  Storage.remove(tempCoverPath.c_str());

  if (thumbOk && outThumbPath != nullptr && outThumbPathLen > 0) {
    strlcpy(outThumbPath, thumbPath.c_str(), outThumbPathLen);
  }

  LOG_DBG("BFC", "BookFusion cover cache result: thumb=%d fit=%d crop=%d", thumbOk, fitOk, cropOk);
  return thumbOk;
}

}  // namespace BookFusionCoverCache
