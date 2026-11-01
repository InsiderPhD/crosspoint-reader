#include "SleepCover.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointState.h"

namespace SleepCover {

bool resolveBmpPath(const GfxRenderer& renderer, const bool cropped, std::string& outPath) {
  LOG_DBG("SLP", "resolveBmpPath: openEpubPath='%s'", APP_STATE.openEpubPath.c_str());

  if (APP_STATE.openEpubPath.empty()) {
    LOG_DBG("SLP", "No cover: openEpubPath is empty");
    return false;
  }

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return false;
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return false;
    }

    outPath = lastXtc.getCoverBmpPath();
    return true;
  }

  if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return false;
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return false;
    }

    outPath = lastTxt.getCoverBmpPath();
    return true;
  }

  if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    LOG_DBG("SLP", "EPUB path; loading metadata + ensuring cover bmp (cropped=%d)", cropped);
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here. The framebuffer is lent to
    // the load's inflate as its 32KB dictionary (heap may have no contiguous block
    // left this deep into a session); released before any drawing below.
    bool lastEpubLoaded;
    {
      InflateScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
      lastEpubLoaded = lastEpub.load(true, true);
    }
    if (!lastEpubLoaded) {
      LOG_ERR("SLP", "Failed to load last epub");
      return false;
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return false;
    }

    outPath = lastEpub.getCoverBmpPath(cropped);
    LOG_DBG("SLP", "Cover bmp ready at: %s", outPath.c_str());
    return true;
  }

  LOG_DBG("SLP", "No cover: unsupported file extension for %s", APP_STATE.openEpubPath.c_str());
  return false;
}

}  // namespace SleepCover
