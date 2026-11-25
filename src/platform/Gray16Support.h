#pragma once

// The board header first: it defaults every FREEINK_DEVICE_* it was not given
// to 0 (an env that -U's one leaves it undefined on the command line), so the
// values below are the same in every translation unit.
#include <BoardConfig.h>

// Which 16-level grey paths this build carries. Both are also gated at runtime on the
// panel the boot probe found (HalDisplay::supportsGray16Planes / supportsGray16Image),
// so a binary that serves two boards (the C3 X3+X4 image) only runs them where the
// driver answers.
//
// CROSSPOINT_GRAY16_PLANES: the UC8253 X3's two-pass planes (freeink-sdk PR #220),
// streamed plane by plane from the one 48 KB framebuffer. No PSRAM needed; the
// reader re-renders the page once per plane (lib/GfxRenderer/Gray16Planes.h).
#if FREEINK_DEVICE_X3
#define CROSSPOINT_GRAY16_PLANES 1
#else
#define CROSSPOINT_GRAY16_PLANES 0
#endif

// CROSSPOINT_GRAY16_IMAGE: the 4 bpp image API (Picco SSD2677 with its two-step text
// turn; UC8179 X4 Pro images only, from jpirnay's calibration). A full-screen level
// buffer is 192 KB, so PSRAM boards only.
#if FREEINK_DEVICE_PICCO || FREEINK_DEVICE_X4PRO
#define CROSSPOINT_GRAY16_IMAGE 1
#else
#define CROSSPOINT_GRAY16_IMAGE 0
#endif
