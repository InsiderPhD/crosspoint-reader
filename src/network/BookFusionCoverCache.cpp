#include "BookFusionCoverCache.h"

#include <Bitmap.h>
#include <Epub.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include <cstdint>
#include <cstring>

#include "components/UITheme.h"
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

// One thumbnail at one theme cover height. Width is the same 3:5 bound
// Epub::generateThumbBmp uses; the converter fits within it and preserves the
// source aspect, so this is a bounding box and not the output size.
bool convertThumbAtHeight(const std::string& srcPath, const Epub& epub, int height) {
  if (height <= 0) return false;
  return convertBookFusionCoverImage(srcPath, epub.getThumbBmpPath(height), true, static_cast<int>(height * 0.6f),
                                     height, true);
}

}  // namespace

namespace BookFusionCoverCache {

std::string withResizeParams(const std::string& url, const int width, const int height) {
  if (url.empty() || width <= 0 || height <= 0) return url;
  // Leave a URL that already carries sizing hints alone.
  if (url.find("width=") != std::string::npos || url.find("height=") != std::string::npos) return url;

  char params[48];
  snprintf(params, sizeof(params), "%cwidth=%d&height=%d", url.find('?') == std::string::npos ? '?' : '&', width,
           height);
  return url + params;
}

bool download(const std::string& coverUrlRaw, const Epub& epub, const int maxWidth, const int maxHeight) {
  const std::string coverUrl = normalizeBookFusionCoverUrl(coverUrlRaw.c_str());
  if (coverUrl.empty()) {
    LOG_DBG("BFC", "No BookFusion cover URL to refresh for %s", epub.getCachePath().c_str());
    return false;
  }

  const std::string tempCoverPath = epub.getCachePath() + "/.bookfusion-cover";
  // Ask for a server-scaled cover. The original artwork on some titles is large
  // enough that JpegToBmpConverter rejects it outright (source capped at
  // 2048x3072, JpegToBmpConverter.cpp:564), leaving those books with no usable
  // cover at all; a scaled fetch avoids that and saves the bytes too.
  const std::string resizedUrl = withResizeParams(coverUrl, maxWidth, maxHeight);

  // Preferred: the pre-scaled cover. Falls back to the original so an
  // unsupported resize parameter costs one failed request, never a missing
  // cover.
  if (resizedUrl != coverUrl) {
    LOG_DBG("BFC", "Downloading resized BookFusion cover: %s", resizedUrl.c_str());
    if (HttpDownloader::downloadToFile(resizedUrl, tempCoverPath, nullptr, false) == HttpDownloader::OK) {
      return true;
    }
    LOG_DBG("BFC", "Resized cover request failed; falling back to the full-size URL");
  }

  LOG_DBG("BFC", "Downloading BookFusion API cover into %s (%s)", epub.getCachePath().c_str(), coverUrl.c_str());
  if (HttpDownloader::downloadToFile(coverUrl, tempCoverPath, nullptr, false) != HttpDownloader::OK) {
    LOG_ERR("BFC", "Failed to download BookFusion API cover into %s", epub.getCachePath().c_str());
    return false;
  }
  return true;
}

bool convert(const Epub& epub, int coverHeight, char* outThumbPath, size_t outThumbPathLen) {
  const std::string tempCoverPath = epub.getCachePath() + "/.bookfusion-cover";

  const std::string thumbPath = epub.getThumbBmpPath(coverHeight);
  const bool thumbOk = convertThumbAtHeight(tempCoverPath, epub, coverHeight);

  // Prime the thumbnail for every other theme's cover height too, while the
  // downloaded image is still on disk. These books have no usable artwork
  // inside the EPUB, so a height that is missing when the user later switches
  // theme cannot be regenerated: Epub::generateThumbBmp finds nothing, writes
  // its zero-byte "already tried" sentinel, and the cover is stuck blank until
  // the next refresh. Each height is one more decode of the same file, which
  // is why this is done here and not on demand.
  const int* heights = UITheme::getCoverThumbHeights();
  for (size_t i = 0; i < UITheme::COVER_THUMB_HEIGHT_COUNT; ++i) {
    if (heights[i] == coverHeight) continue;
    convertThumbAtHeight(tempCoverPath, epub, heights[i]);
  }

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

bool refresh(const std::string& coverUrlRaw, const Epub& epub, int coverHeight, char* outThumbPath,
             size_t outThumbPathLen) {
  if (!download(coverUrlRaw, epub, kCoverFetchWidth, kCoverFetchHeight)) return false;
  return convert(epub, coverHeight, outThumbPath, outThumbPathLen);
}

}  // namespace BookFusionCoverCache
