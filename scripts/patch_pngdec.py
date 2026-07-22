"""
PlatformIO pre-build script: neutralize PNGdec's ESP32-S3 SIMD assembly on C3.

Problem:
  PNGdec ships src/s3_simd_rgb565.S, hand-written Xtensa vector assembly
  (ee.vld.128, q0..q7) that only exists on the ESP32-S3.  Its guard is merely
  `#ifdef ARDUINO_ARCH_ESP32`, which is defined on ALL ESP32 variants including
  our single-core RISC-V ESP32-C3.  So the file gets assembled for the C3 and
  immediately fails on `#include "dsps_fft2r_platform.h"` — an ESP-DSP/S3 header
  that does not exist for the C3 target:

    fatal error: dsps_fft2r_platform.h: No such file or directory

Fix:
  Tighten the guard to also require CONFIG_IDF_TARGET_ESP32S3, so the entire
  file compiles to nothing on the C3 (and any non-S3 target).  The SIMD path is
  genuinely S3-only; the C3 build already uses PNGdec's portable C fallback.

Applied idempotently — safe to run on every build.
"""

Import("env")
import os


def patch_pngdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        s3_asm = os.path.join(libdeps_dir, env_dir, "PNGdec", "src", "s3_simd_rgb565.S")
        if os.path.isfile(s3_asm):
            _apply_s3_guard_fix(s3_asm)


def _apply_s3_guard_fix(filepath):
    MARKER = "// CrossPoint patch: S3-only SIMD guard"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    OLD = "#ifdef ARDUINO_ARCH_ESP32"
    NEW = (
        MARKER + "\n"
        "#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32S3)"
    )

    if OLD not in content:
        print(
            "WARNING: PNGdec S3 SIMD guard patch target not found in %s "
            "— library may have been updated" % filepath
        )
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched PNGdec: S3-only SIMD guard in s3_simd_rgb565.S: %s" % filepath)


# Run immediately at script import time (before compilation).
patch_pngdec(env)
