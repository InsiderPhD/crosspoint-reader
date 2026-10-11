"""
PlatformIO pre-build script: inline patches for the pinned JPEGDEC.

--- Patch 1: MCU_SKIP wild pointer crash ---

Problem:
  JPEGDecodeMCU_P computes pMCU = &sMCUs[iMCU & 0xffffff].  When iMCU is
  MCU_SKIP (-8), the bitmask produces index 0xFFFFF8 (16 777 208), creating a
  pointer ~33 MB past the 392-entry sMCUs array.  If the progressive JPEG's
  first scan includes AC coefficients (iScanEnd > 0), the AC decode loop writes
  through this wild pointer and crashes with a store-access fault.

  Upstream commit 8628297 guarded the DC coefficient write (pMCU[0]) but not the
  AC coefficient writes at indices 1-63.

Fix:
  Redirect pMCU to sMCUs[0] when MCU_SKIP is active.  Writes to sMCUs[1..63]
  are harmless: for JPEG_SCALE_EIGHTH only sMCUs[0] is read for output, and
  the DC write at sMCUs[0] is already guarded by the existing `if (iMCU >= 0)`
  check.

--- Patch 2: progressive JPEGs with separated component scans ---

Problem:
  With EIGHT_BIT_GRAYSCALE output DecodeJPEG still has to consume the chroma
  entropy data, so after each luma MCU it calls JPEGDecodeMCU_P(MCU_SKIP) once
  for Cb and once for Cr, unconditionally.  A progressive file is free to put
  each component in its own scan (non-interleaved); in a luma-only scan there
  is no chroma data at all, so the two skip calls eat luma bits, the bitstream
  desynchronises and the image decodes blank.  When Cr is present its skip call
  also ran with the Cb Huffman tables still selected.

Fix (upstream crosspoint-reader 6230eba2d):
  Only skip a chroma component whose JPCI[n].component_needed is set for the
  current scan, and select cACTable2/cDCTable2 before skipping the second one.

Both are applied idempotently — safe to run on every build.
"""

Import("env")
import os


def patch_jpegdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        jpeg_inl = os.path.join(libdeps_dir, env_dir, "JPEGDEC", "src", "jpeg.inl")
        if os.path.isfile(jpeg_inl):
            _apply_mcu_skip_pointer_fix(jpeg_inl)
            _apply_separated_scan_fix(env, jpeg_inl)


def _apply_mcu_skip_pointer_fix(filepath):
    MARKER = "// CrossPoint patch: safe pMCU for MCU_SKIP"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    # The wild-pointer line in JPEGDecodeMCU_P:
    OLD = "    signed short *pMCU = &pJPEG->sMCUs[iMCU & 0xffffff];"

    NEW = (
        "    " + MARKER + "\n"
        "    signed short *pMCU = (iMCU < 0) ? pJPEG->sMCUs\n"
        "                                     : &pJPEG->sMCUs[iMCU & 0xffffff];"
    )

    if OLD not in content:
        print(
            "WARNING: JPEGDEC MCU_SKIP pointer patch target not found in %s "
            "— library may have been updated" % filepath
        )
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: safe pMCU for MCU_SKIP in JPEGDecodeMCU_P: %s" % filepath)


def _apply_separated_scan_fix(env, filepath):
    MARKER = "// CrossPoint patch: skip only chroma present in this scan"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    # The unconditional chroma skips in DecodeJPEG's grayscale branch.
    OLD = (
        "                        iErr |= JPEGDecodeMCU_P(pJPEG, MCU_SKIP, &iDCPred1);\n"
        "                        iErr |= JPEGDecodeMCU_P(pJPEG, MCU_SKIP, &iDCPred2);\n"
    )

    NEW = (
        "                        " + MARKER + "\n"
        "                        if (pJPEG->JPCI[1].component_needed)\n"
        "                            iErr |= JPEGDecodeMCU_P(pJPEG, MCU_SKIP, &iDCPred1);\n"
        "                        if (pJPEG->JPCI[2].component_needed) {\n"
        "                            pJPEG->ucACTable = cACTable2;\n"
        "                            pJPEG->ucDCTable = cDCTable2;\n"
        "                            iErr |= JPEGDecodeMCU_P(pJPEG, MCU_SKIP, &iDCPred2);\n"
        "                        }\n"
    )

    # Unlike patch 1 this one fails the build: a silently unpatched decoder
    # renders these covers blank, which looks like a cache or image bug.
    if content.count(OLD) != 1:
        print(
            "ERROR: JPEGDEC separated-scan patch anchor found %d times (expected 1) in %s "
            "- the JPEGDEC pin changed; re-review scripts/patch_jpegdec.py"
            % (content.count(OLD), filepath)
        )
        env.Exit(1)

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: skip only chroma present in this scan: %s" % filepath)


# Run immediately at script import time (before compilation).
patch_jpegdec(env)
