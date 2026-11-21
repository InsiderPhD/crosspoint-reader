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

Fix (v2):
  Redirect pMCU to a private static 64-coefficient scratch block when
  MCU_SKIP is active.  Skipped coefficients are decoded only to advance the
  bitstream, so where they land does not matter -- as long as it is not a live
  block.

  v1 redirected them to sMCUs[0] on the assumption the writes were harmless.
  They are not: the DC store (pMCU[0] = *iDCPredictor) is unguarded, and
  sMCUs[0] is luma block 0, already filled with its output bytes when the
  EIGHT_BIT_GRAYSCALE path skips Cb/Cr.  Every progressive 4:2:0 image decoded
  at 1/8 had the top-left pixel of each MCU replaced by the low byte of a
  chroma DC predictor -- a lattice of dark squares across sleep covers, which
  the bilinear cover upscale then enlarged.  v1-patched copies are upgraded.

--- Patch 3: bound the progressive scan range ---

Problem:
  JPEGGetSOS stores Ss/Se straight from the file, and the AC loops in
  JPEGDecodeMCU_P write pMCU[iIndex] for iIndex up to Se.  A malformed Se
  (up to 255) writes past the 64-coefficient block -- past the patch-1
  scratch block, or past sMCUs for a live block.

Fix:
  Reject a scan with Se > 63 or Ss > Se as bogus (the spec caps Se at 63),
  as JPEGGetSOS already does for bad table numbers.

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

All are applied idempotently — safe to run on every build.
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
            _apply_mcu_skip_pointer_fix(env, jpeg_inl)
            _apply_separated_scan_fix(env, jpeg_inl)
            _apply_scan_bound_fix(env, jpeg_inl)


def _apply_mcu_skip_pointer_fix(env, filepath):
    MARKER = "// CrossPoint patch v2: skipped MCUs decode into private scratch"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    # The wild-pointer line in JPEGDecodeMCU_P, as pinned:
    ORIGINAL = "    signed short *pMCU = &pJPEG->sMCUs[iMCU & 0xffffff];"
    # The same line after patch v1, which aliased luma block 0 (see docstring):
    V1 = (
        "    // CrossPoint patch: safe pMCU for MCU_SKIP\n"
        "    signed short *pMCU = (iMCU < 0) ? pJPEG->sMCUs\n"
        "                                     : &pJPEG->sMCUs[iMCU & 0xffffff];"
    )

    # Static, not stack: this runs once per 8x8 block. 128 B of .bss; never
    # read, so concurrent decoders sharing it is harmless. Patch 3 bounds the
    # writes to indices 0..63.
    NEW = (
        "    " + MARKER + "\n"
        "    static signed short sSkipMCU[DCTSIZE];\n"
        "    signed short *pMCU = (iMCU < 0) ? sSkipMCU\n"
        "                                     : &pJPEG->sMCUs[iMCU & 0xffffff];"
    )

    if content.count(V1) == 1:
        content = content.replace(V1, NEW, 1)
    elif content.count(ORIGINAL) == 1:
        content = content.replace(ORIGINAL, NEW, 1)
    else:
        # Fail the build: unpatched, progressive covers crash; v1-patched, they
        # decode with a lattice of dark squares.
        print(
            "ERROR: JPEGDEC MCU_SKIP pointer patch anchor not found in %s "
            "- the JPEGDEC pin changed; re-review scripts/patch_jpegdec.py" % filepath
        )
        env.Exit(1)
        return

    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: skipped MCUs decode into private scratch: %s" % filepath)


def _apply_scan_bound_fix(env, filepath):
    MARKER = "// CrossPoint patch: bound the scan range (spec: Se <= 63)"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    OLD = "    pJPEG->iScanEnd = buf[iOffset++]; // Get the scan end for this scan\n"
    NEW = OLD + (
        "    " + MARKER + "\n"
        "    if (pJPEG->iScanEnd > 63 || pJPEG->iScanStart > pJPEG->iScanEnd)\n"
        "        return 1; // bogus scan range\n"
    )

    if content.count(OLD) != 1:
        print(
            "ERROR: JPEGDEC scan-bound patch anchor found %d times (expected 1) in %s "
            "- the JPEGDEC pin changed; re-review scripts/patch_jpegdec.py" % (content.count(OLD), filepath)
        )
        env.Exit(1)
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: bound the scan range: %s" % filepath)


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
