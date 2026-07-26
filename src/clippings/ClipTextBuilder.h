#pragma once

#include "activities/ActivityResult.h"
#include "activities/reader/WordRef.h"

// Builds the cleaned clipping text (and re-location anchors) for the selected word range
// [from, to] out of the transient per-page WordList produced by the reader.
namespace ClipTextBuilder {

ClippingResult build(const WordList& wordList, int from, int to, int total, int startPageInSection,
                     int sectionPageCount);

}  // namespace ClipTextBuilder
