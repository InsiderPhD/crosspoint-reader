#include "CoverCache.h"

#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "RecentBooksStore.h"
#include "components/UITheme.h"

namespace CoverCache {

void fitSize(const Bitmap& bitmap, const int boxW, const int boxH, int& wOut, int& hOut) {
  const float sx = static_cast<float>(boxW) / static_cast<float>(bitmap.getWidth());
  const float sy = static_cast<float>(boxH) / static_cast<float>(bitmap.getHeight());
  const float scale = std::min(sx, sy);
  wOut = std::clamp(static_cast<int>(bitmap.getWidth() * scale), 1, boxW);
  hOut = std::clamp(static_cast<int>(bitmap.getHeight() * scale), 1, boxH);
}

namespace {

// Sampled from two points in the source's pixel data for the stamp below.
constexpr size_t kStampSampleBytes = 64;

// Identifies the source thumbnail a cached cover was made from, stored
// in the cached BMP's two reserved header fields. File size alone is not
// enough: a regenerated cover (Regenerate Cover, a BookFusion refresh) almost
// always has the same dimensions and so the same size. Leaves src at 0.
uint32_t sourceStamp(FsFile& src) {
  const size_t size = static_cast<size_t>(src.size());
  uint32_t h = 2166136261u;
  for (int i = 0; i < 4; ++i) h = (h ^ ((size >> (8 * i)) & 0xFFu)) * 16777619u;
  uint8_t sample[kStampSampleBytes];
  for (const size_t at : {size / 3, (2 * size) / 3}) {
    if (!src.seek(at)) continue;
    const int got = src.read(sample, sizeof(sample));
    for (int i = 0; i < got; ++i) h = (h ^ sample[i]) * 16777619u;
  }
  src.seek(0);
  return h;
}

Rect placeCover(const CoverPlacement& place, const int w, const int h) {
  const int x = place.centerX ? place.box.x + (place.box.width - w) / 2 : place.box.x;
  const int y = place.bottomAlign ? place.box.y + place.box.height - h : place.box.y;
  return Rect{x, y, w, h};
}

// thumb_[HEIGHT].bmp -> thumb_<key>_<w>x<h>.bmp, keyed by use and box size so a
// different layout (landscape, gesture mode) gets its own copy. False when the
// cover path has no [HEIGHT] template, since the cache would then overwrite
// the source itself.
bool coverCachePath(const std::string& coverTemplate, const CoverPlacement& place, std::string& out) {
  const size_t pos = coverTemplate.find("[HEIGHT]");
  if (pos == std::string::npos) return false;
  char key[32];
  snprintf(key, sizeof(key), "%s_%dx%d", place.cacheKey, place.box.width, place.box.height);
  out = coverTemplate;
  out.replace(pos, 8, key);
  return true;
}

// Draws the cached cover if one exists for this stamp and fits the box. False
// on any mismatch, so the caller rebuilds it.
bool drawCachedCover(const GfxRenderer& renderer, const std::string& cachePath, const uint32_t stamp,
                     const CoverPlacement& place, Rect& drawnOut) {
  if (!Storage.exists(cachePath.c_str())) return false;
  FsFile file;
  if (!Storage.openFileForRead("HOME", cachePath, file)) return false;

  // bfType at 0, bfReserved1/2 at 6/8. memcpy: RISC-V faults on unaligned loads.
  uint8_t header[14];
  bool ok =
      file.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) && header[0] == 'B' && header[1] == 'M';
  if (ok) {
    uint16_t lo = 0;
    uint16_t hi = 0;
    memcpy(&lo, header + 6, sizeof(lo));
    memcpy(&hi, header + 8, sizeof(hi));
    ok = ((static_cast<uint32_t>(hi) << 16) | lo) == stamp && file.seek(0);
  }
  if (ok) {
    Bitmap bitmap(file);
    ok = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.is1Bit() && bitmap.getWidth() > 0 &&
         bitmap.getHeight() > 0 && bitmap.getWidth() <= place.box.width && bitmap.getHeight() <= place.box.height;
    if (ok) {
      drawnOut = placeCover(place, bitmap.getWidth(), bitmap.getHeight());
      renderer.drawBitmap(bitmap, drawnOut.x, drawnOut.y, drawnOut.width, drawnOut.height);  // 1:1, no scaling
    }
  }
  file.close();
  return ok;
}

// Shrinks the source thumbnail into the box, draws it, and writes the result
// to cachePath (skipped when cachePath is empty). The 1-bit buffer is a heap
// temporary freed before returning -- ~0.9 KB for a shelf slot, ~11 KB for the
// hero cover, past the 256 B stack budget either way. A failed allocation
// falls back to drawing directly, uncached.
bool drawAndCacheCover(const GfxRenderer& renderer, FsFile& src, const std::string& cachePath, const uint32_t stamp,
                       const CoverPlacement& place, Rect& drawnOut) {
  Bitmap bitmap(src);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) return false;
  int w = 0;
  int h = 0;
  fitSize(bitmap, place.box.width, place.box.height, w, h);
  drawnOut = placeCover(place, w, h);

  const int rowBytes = ((w + 31) / 32) * 4;  // BMP rows pad to 4 bytes
  const size_t bufSize = static_cast<size_t>(rowBytes) * h;
  auto* bits = static_cast<uint8_t*>(malloc(bufSize));
  if (bits == nullptr) {
    LOG_ERR("HOME", "cover cache malloc failed: %d bytes", static_cast<int>(bufSize));
    renderer.drawBitmapResampled(bitmap, drawnOut.x, drawnOut.y, w, h);
    return true;
  }
  if (!GfxRenderer::resampleBitmapTo1Bit(bitmap, w, h, bits, rowBytes)) {
    free(bits);
    return false;
  }

  for (int dy = 0; dy < h; ++dy) {
    const uint8_t* row = bits + dy * rowBytes;
    for (int dx = 0; dx < w; ++dx) {
      if ((row[dx / 8] & (0x80u >> (dx % 8))) == 0) renderer.drawPixel(drawnOut.x + dx, drawnOut.y + dy);
    }
  }

  if (!cachePath.empty()) {
    BmpHeader header;
    createBmpHeader(&header, w, h, BmpRowOrder::TopDown);
    header.fileHeader.bfReserved1 = static_cast<uint16_t>(stamp & 0xFFFFu);
    header.fileHeader.bfReserved2 = static_cast<uint16_t>(stamp >> 16);
    FsFile out;
    if (Storage.openFileForWrite("HOME", cachePath, out)) {
      const bool written = out.write(&header, sizeof(header)) == sizeof(header) && out.write(bits, bufSize) == bufSize;
      out.close();
      if (!written) {
        LOG_ERR("HOME", "cover cache write failed: %s", cachePath.c_str());
        Storage.remove(cachePath.c_str());  // a short file would fail parse forever
      }
    }
  }
  free(bits);
  bits = nullptr;
  return true;
}

}  // namespace

bool drawCoverCached(const GfxRenderer& renderer, const RecentBook& book, const CoverPlacement& place,
                     const int thumbHeight, Rect& drawnOut) {
  if (book.coverBmpPath.empty()) return false;
  const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
  if (!Storage.exists(thumbPath.c_str())) return false;
  FsFile src;
  if (!Storage.openFileForRead("HOME", thumbPath, src)) return false;

  std::string cachePath;
  const bool cacheable = coverCachePath(book.coverBmpPath, place, cachePath);
  const uint32_t stamp = cacheable ? sourceStamp(src) : 0;
  bool ok = cacheable && drawCachedCover(renderer, cachePath, stamp, place, drawnOut);
  if (!ok) {
    ok = drawAndCacheCover(renderer, src, cacheable ? cachePath : std::string(), stamp, place, drawnOut);
  }
  src.close();
  if (ok) {
    renderer.maskRoundedRectOutsideCorners(drawnOut.x, drawnOut.y, drawnOut.width, drawnOut.height, place.radius,
                                           Color::White);
    renderer.drawRoundedRect(drawnOut.x, drawnOut.y, drawnOut.width, drawnOut.height, 1, place.radius, true);
  }
  return ok;
}

}  // namespace CoverCache
