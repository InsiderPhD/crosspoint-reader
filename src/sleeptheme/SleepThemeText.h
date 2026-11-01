#pragma once

#include <cstddef>

#include "SleepThemeFormat.h"

namespace SleepThemeText {

// CSS text-transform on a UTF-8 string, into out (always NUL-terminated).
// Case pairs are mapped for Latin (ASCII, Latin-1, Latin Extended-A), Greek
// and Cyrillic; other scripts have no case and pass through unchanged.
void applyTransform(SleepThemeFormat::TextTransform transform, const char* in, char* out, size_t outSize);

}  // namespace SleepThemeText
