"""
Patch the pinned AnimatedGIF (bitbank2, 67cfacaf) before it is compiled.

Adapted from CrossPoint #2299's scripts/patch_animatedgif.py:

1. MAX_WIDTH (the decoder's scanline buffer, part of the one ~26 KB AnimatedGIF
   block) is taken from CROSSPOINT_GIF_MAX_WIDTH instead of the library's fixed
   480. Not a global -DMAX_WIDTH: firmware code has its own MAX_WIDTH names
   (CssBorderSide::MAX_WIDTH) that a command-line macro would break.
2. Unaligned multi-word copies stay off on RISC-V (ESP32-C3): the library turns
   them on for every ESP32, and the C3 is not to be trusted with unaligned
   loads (see CLAUDE.md "RISC-V Alignment"). The byte-at-a-time fallback is the
   library's own code path.
3. The global palette read and the scanline copy are bounded (upstream fixes).

Idempotent: every edit is skipped when its result is already present.
"""

import argparse
import os
import sys


WIDTH_OLD = """#define MAX_COLORS 256
#ifdef __LINUX__
#define MAX_WIDTH 2048
#else
#define MAX_WIDTH 480
#endif // __LINUX__
"""

WIDTH_NEW = """#define MAX_COLORS 256
#ifndef MAX_WIDTH
#if defined(CROSSPOINT_GIF_MAX_WIDTH)
#define MAX_WIDTH CROSSPOINT_GIF_MAX_WIDTH
#elif defined(__LINUX__)
#define MAX_WIDTH 2048
#else
#define MAX_WIDTH 480
#endif
#endif // MAX_WIDTH
"""

UNALIGNED_OLD = """#if defined(HAL_ESP32_HAL_H_) || defined(TEENSYDUINO) || defined(ARM_MATH_CM4) || defined(ARM_MATH_CM7)
#define ALLOWS_UNALIGNED
#endif
"""

UNALIGNED_NEW = """#if (defined(HAL_ESP32_HAL_H_) && !defined(__riscv)) || defined(TEENSYDUINO) || defined(ARM_MATH_CM4) || defined(ARM_MATH_CM7)
#define ALLOWS_UNALIGNED
#endif
"""

PALETTE_OLD = """            // Read enough additional data for the color table
            i = (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], 3*(1<<iColorTableBits));
            iBytesRead += i;
            if (iColorTableBits != 1 && i < 3*(1<<iColorTableBits)) { // file too small
                pPage->iError = GIF_BAD_FILE;
                return 0;
            }
"""

PALETTE_NEW = """            // The initial read may already contain the complete color table.
            // Keep the existing read-ahead for the following block parser.
            i = (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], 3*(1<<iColorTableBits));
            iBytesRead += i;
            if (iBytesRead < iOffset + 3*(1<<iColorTableBits)) {
                pPage->iError = GIF_BAD_FILE;
                return 0;
            }
"""

COPY_OLD = """            while (buf < pEnd) {
#ifdef ALLOWS_UNALIGNED
// This is a significant perf improvement compared to copying 1 byte at a time
// even though it will often copy too many bytes. Since we're not at the end of
// the line, it's okay to copy a few extra pixels.
                BIGUINT tmp = *(BIGUINT *) s;
                s += sizeof(BIGUINT);
                *(BIGUINT *)buf = tmp;
                buf += sizeof(BIGUINT);
#else
                *buf++ = *s++;
#endif
            }
"""

COPY_NEW = """#ifdef ALLOWS_UNALIGNED
            while (pEnd - buf >= (int)sizeof(BIGUINT)) {
                BIGUINT tmp = *(BIGUINT *) s;
                s += sizeof(BIGUINT);
                *(BIGUINT *)buf = tmp;
                buf += sizeof(BIGUINT);
            }
#endif
            while (buf < pEnd) {
                *buf++ = *s++;
            }
"""


def _apply(path, edits):
    with open(path, "r", encoding="utf-8") as file:
        original = file.read()
    text = original
    for name, before, after in edits:
        if after in text:
            continue
        if text.count(before) != 1:
            sys.stderr.write("ERROR: AnimatedGIF %s patch does not match %s\n" % (name, path))
            raise SystemExit(1)
        text = text.replace(before, after, 1)
    if text != original:
        with open(path, "w", encoding="utf-8") as file:
            file.write(text)
        print("Patched AnimatedGIF: %s" % os.path.basename(path))


def patch_library(library_path):
    _apply(
        os.path.join(library_path, "src", "AnimatedGIF.h"),
        (("MAX_WIDTH", WIDTH_OLD, WIDTH_NEW), ("RISC-V unaligned", UNALIGNED_OLD, UNALIGNED_NEW)),
    )
    _apply(
        os.path.join(library_path, "src", "gif.inl"),
        (("global palette", PALETTE_OLD, PALETTE_NEW), ("bounded scanline copy", COPY_OLD, COPY_NEW)),
    )
    return True


def patch_animatedgif(env, require_found=False):
    library_path = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "AnimatedGIF")
    patched = os.path.isdir(library_path) and patch_library(library_path)
    if require_found and not patched:
        sys.stderr.write("ERROR: AnimatedGIF dependency was not found to patch\n")
        raise SystemExit(1)
    return patched


def patch_animatedgif_before_link(source, target, env):
    patch_animatedgif(env, require_found=True)


if "Import" in globals():
    Import("env")  # noqa: F821 (SCons-injected global)
    patch_animatedgif(env, require_found=False)  # noqa: F821
    env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", patch_animatedgif_before_link)  # noqa: F821
elif __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library_path")
    patch_library(parser.parse_args().library_path)
