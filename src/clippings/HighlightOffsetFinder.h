#pragma once

#include <Epub.h>

#include <cstdint>
#include <string>

// Locates a clipping's text in the raw XHTML of its spine item and reports it
// the way BookFusion anchors highlights: start/end offsets in UTF-16 code units
// from the start of the <body> element's text content (end exclusive) — the
// same unit the KOReader plugin computes in bf_epub.lua xpointerToOffset.
//
// The clip text is whitespace-collapsed and de-hyphenated by the reader, so the
// match ignores whitespace, hyphens and ASCII case on both sides. When the text
// occurs more than once, the occurrence nearest expectedFraction (0..1 through
// the chapter) wins.
//
// Inflates the whole chapter: the caller must hold an InflateScratchLease (or
// have 32KB contiguous free) and must not render while this runs.
namespace HighlightOffsetFinder {

enum class Result : uint8_t {
  Found,
  NotFound,  // chapter read fine, text isn't in it — retrying won't help
  Error,     // allocation / read failure — worth retrying later
};

Result find(const Epub& epub, int spineIndex, const std::string& clipText, float expectedFraction, uint32_t& outStart,
            uint32_t& outEnd);

}  // namespace HighlightOffsetFinder
