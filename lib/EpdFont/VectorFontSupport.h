#pragma once

// Resolves CROSSPOINT_VECTOR_FONTS (runtime TrueType/OpenType reader fonts,
// PSRAM boards only) for the font library.
//
// The switch changes the layout of EpdFontData and SdCardFontFamilyInfo, so
// every translation unit must agree on it. lib/DevicePolicy owns the
// definition; the fallback derives it from the same build flag, for host unit
// tests (and any library build) that compile these headers without
// lib/DevicePolicy on the include path.
#if __has_include(<DevicePolicy.h>)
#include <DevicePolicy.h>
#endif
#ifndef CROSSPOINT_VECTOR_FONTS
#if defined(BOARD_HAS_PSRAM)
#define CROSSPOINT_VECTOR_FONTS 1
#else
#define CROSSPOINT_VECTOR_FONTS 0
#endif
#endif
