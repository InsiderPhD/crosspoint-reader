#include "DictionaryLookup.h"

#include <GfxRenderer.h>
#include <InflateReader.h>
#include <Logging.h>

#include "CrossPointSettings.h"

namespace {

// Fed to buildIndex() every ~64KB consumed. The index pass is a long streaming
// read on the loop task, which the watchdog would otherwise treat as a wedge.
void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

bool DictionaryLookup::prepare() {
  if (openAttempted) return openOk;
  openAttempted = true;

  if (SETTINGS.dictionaryName[0] == '\0') {
    openOk = false;
    return false;
  }
  openOk = dict.open(SETTINGS.dictionaryName);
  // needsIndex() opens and validates the sidecars, so ask once per open rather
  // than once per word — the answer only changes when run() builds them itself.
  needsIndex = openOk && dict.needsIndex();
  return openOk;
}

DictionaryLookup::Outcome DictionaryLookup::run(GfxRenderer& renderer, const char* word) {
  Outcome out;

  if (!prepare()) {
    out.message = StrId::STR_DICT_ERROR;
    return out;
  }

  // Held across the index build AND the lookup: both inflate, and taking the
  // lease once avoids handing the framebuffer back only to want it again.
  InflateScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());

  if (needsIndex) {
    Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
    const bool built = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    // A successful build leaves the sidecars fresh; a failed one is retried on
    // the next lookup rather than poisoning the session.
    needsIndex = !built;
    if (!built) {
      // An index build allocates a scan buffer, so it fails the same way lookups
      // do on a fragmented heap. Name that rather than a generic error.
      out.message =
          indexResult == Dictionary::IndexResult::LowMemory ? StrId::STR_DICT_LOW_MEMORY : StrId::STR_DICT_READ_FAILED;
      return out;
    }
  }

  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  if (dict.lookup(word, out.definition, out.headword, &result)) {
    out.found = true;
    out.html = dict.definitionsAreHtml();
    return out;
  }

  switch (result) {
    case Dictionary::LookupResult::Decompress:
      out.message = StrId::STR_DICT_DECOMPRESS_ERROR;
      break;
    case Dictionary::LookupResult::LowMemory:
      out.message = StrId::STR_DICT_LOW_MEMORY;
      break;
    case Dictionary::LookupResult::ReadError:
      out.message = StrId::STR_DICT_READ_FAILED;
      break;
    case Dictionary::LookupResult::NotFound:
    default:
      out.message = StrId::STR_DICT_NOT_FOUND;
      break;
  }
  return out;
}
