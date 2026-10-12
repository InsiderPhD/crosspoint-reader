from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"

# -DCROSSPOINT_WOLFSSL_SP_4096=1 in an env's build_flags lifts the bignum cap
# from 3072 to 4096 bits. Without it SP math (SP_INT_BITS, sp_int.h) sizes its
# integers for 3072-bit RSA/DH at most, and a server presenting a 4096-bit RSA
# certificate fails the handshake with MP_INIT_E (-110): sp_init_size refuses
# an integer wider than SP_INT_DIGITS while the ServerKeyExchange / TLS 1.3
# CertificateVerify signature is checked. Seen 2026-10-02 against a plugin
# catalog host (Let's Encrypt, 4096-bit key); BookFusion/GitHub use 2048/ECDSA.
#
# Cost: RSA temporaries are sized to the key in use (NEW_MP_INT_SIZE), so a
# 2048-bit server pays only for the few fixed-size sp_int members (the peer
# RsaKey's n/e etc.), ~256 bytes each; a 4096-bit server then needs ~4-8KB
# more during its verify, plus the sp_c32 4096 code in flash. Set on the PSRAM
# boards (x4pro/x4c); the C3 boards, whose TLS handshake is already at the
# edge of the heap, opt in per env (platformio.local.ini) after measuring.
def wants_sp_4096() -> bool:
    # Pre-build scripts run before PlatformIO folds the project's build_flags
    # into CPPDEFINES, so read the env's resolved build_flags option directly
    # (it already has ${base.build_flags} expanded); keep the CPPDEFINES check
    # for flags injected by other scripts.
    try:
        option = env.GetProjectOption("build_flags", "") or ""
    except Exception:  # noqa: BLE001 - any failure just means "not set here"
        option = ""
    if isinstance(option, (list, tuple)):
        option = " ".join(str(x) for x in option)
    if "CROSSPOINT_WOLFSSL_SP_4096" in str(option):
        return True
    for define in env.get("CPPDEFINES", []):
        name = define[0] if isinstance(define, (tuple, list)) else define
        if str(name) == "CROSSPOINT_WOLFSSL_SP_4096":
            return True
    return False


SP_4096_OVERRIDE = """
/* 4096-bit RSA peers (CROSSPOINT_WOLFSSL_SP_4096): lifts SP_INT_BITS to 4096
 * and compiles the sp_c32 4096 RSA paths. */
#define WOLFSSL_SP_4096
"""

OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif

/* The Arduino user_settings.h selects USE_FAST_MATH, under which every bignum
 * is a fixed FP_MAX_BITS-sized fp_int (~4KB heap alloc with SMALL_STACK) and a
 * single RSA cert verify needs several at once. On the C3's ~40KB free heap
 * that OOMs the handshake (PEER_KEY_ERROR -342 / MP_EXPTMOD_E -112 seen
 * on-device, MinFree 8656). Replace it with SP math: allocations are sized to
 * the actual operand (hundreds of bytes), and the fixed-size SP code paths are
 * what -DWOLFSSL_SP_RISCV32 was always meant to accelerate (it is inert under
 * fastmath). SP_SMALL trades a little speed for the smallest footprint. */
#undef USE_FAST_MATH
#undef FP_MAX_BITS
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_SMALL
#define WOLFSSL_HAVE_SP_RSA
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_HAVE_SP_DH

/* Input record buffer policy, read by the patched ShrinkInputBuffer /
 * GrowInputBuffer in src/internal.c (see scripts/patch_wolfssl.py). Defined
 * here rather than as -D flags so reverting the script also turns the patched
 * branches back into stock wolfSSL without clearing the dependency cache. */
#define CROSSPOINT_WOLFSSL_RETAIN_INPUT_BUFFER
#define CROSSPOINT_WOLFSSL_RELEASE_EMPTY_INPUT_BUFFER
"""


def patch_user_settings(path: Path, sp_4096: bool) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    overrides = OVERRIDES + (SP_4096_OVERRIDE if sp_4096 else "")
    path.write_text(text + overrides + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)} (SP 4096: {'on' if sp_4096 else 'off'})")


# wolfcrypt/settings.h has `#include "RTOS.h"` inside `#ifdef WOLFSSL_EMBOS`
# (SEGGER embOS — never defined on this platform). PlatformIO's chain-mode LDF
# does not evaluate preprocessor guards, so the bare include makes it resolve
# "RTOS.h" to the Arduino framework's bluedroid BLE library, whose libbt.a
# objects then collide at link with our vendored lib/NimBLE-Arduino (multiple
# definition of npl_freertos_*). Upstream sidesteps this with `lib_ignore =
# BLE`, but pioarduino's component_manager reacts to that by stripping the
# framework's bt/* include dirs — which the vendored NimBLE needs. Rewriting
# the include as a computed include hides it from the LDF's regex scanner
# while staying compilable under embOS. Idempotent via the marker line.
RTOS_MARKER = "/* CrossPoint patch: hide embOS RTOS.h from PlatformIO LDF */"
RTOS_OLD = '#ifdef WOLFSSL_EMBOS\n    #include "RTOS.h"'
RTOS_NEW = (
    "#ifdef WOLFSSL_EMBOS\n"
    f"    {RTOS_MARKER}\n"
    '    #define WOLFSSL_EMBOS_RTOS_HEADER "RTOS.h"\n'
    "    #include WOLFSSL_EMBOS_RTOS_HEADER"
)


def patch_embos_rtos_include(path: Path) -> None:
    text = path.read_text()
    if RTOS_MARKER in text:
        return
    if RTOS_OLD not in text:
        print(
            f"WARNING: embOS RTOS.h include not found in {path.relative_to(PROJECT_DIR)} "
            "— wolfSSL layout may have changed; check the BLE/NimBLE link collision"
        )
        return
    path.write_text(text.replace(RTOS_OLD, RTOS_NEW, 1))
    print(f"Patched wolfSSL settings.h: hid embOS RTOS.h include from LDF: {path.relative_to(PROJECT_DIR)}")


# --- Input record buffer: keep it between records, free it before regrowing ---
#
# Stock wolfSSL keeps a 5-byte static input buffer (STATIC_BUFFER_LEN ==
# RECORD_HEADER_SZ), so EVERY record is read into a malloc'd buffer sized to
# that record, and ShrinkInputBuffer frees it again as soon as the record is
# consumed. A server that ignores max_fragment_length sends 16KB records, so a
# download costs one ~16.4KB malloc + ForceZero + free per record: megabytes of
# transfer means hundreds of trips through the allocator for the single largest
# block the session ever needs, each one a fresh chance for a small allocation
# made in between (lwIP pbufs, the HTTP client's strings) to land in the hole
# and leave no 16.4KB run for the next record -> MEMORY_E (-125) mid-transfer.
#
# RETAIN: after the handshake, ShrinkInputBuffer(NO_FORCED_FREE) becomes a
# no-op, so the buffer grown for the first full-size record is reused for every
# later record that fits. One allocation per connection instead of one per
# record, and an under-provisioned heap fails on the first record rather than
# megabytes in.
#   * Gated on HANDSHAKE_DONE, which upstream's patch is not: during the
#     handshake the stock shrink still runs, so the certificate-chain record's
#     buffer is NOT held across the signature verify. The handshake is the
#     tightest moment on the C3 (TlsFramebufferBorrow's 48KB arena carries the
#     session plus every crypto temporary) and its peak stays byte-identical.
#   * Released on close: wolfSSL_free -> SSL_ResourceFree calls
#     ShrinkInputBuffer(ssl, FORCED_FREE), which this patch leaves alone.
#     SecureClient::stop() (and its destructor) call wolfSSL_free, so nothing
#     outlives the connection. Resident cost while a connection is OPEN: the
#     largest record received so far (<= ~16.4KB), held between records where
#     stock wolfSSL would have returned it. Under TlsFramebufferBorrow that
#     block lives in the framebuffer arena, not the heap.
#
# RELEASE_EMPTY: with retention a later, larger record would make
# GrowInputBuffer malloc the new buffer while the old one is still held (old +
# new contiguous at once -- worse than stock). When the old buffer is fully
# consumed and no decrypted plaintext still points into it, free it first, so
# peak contiguous demand is never above stock's single record buffer.
#
# Both branches are compiled only when user_settings.h (rewritten above on
# every build) defines the CROSSPOINT_WOLFSSL_* switches.
SHRINK_MARKER = "/* CrossPoint: keep a grown input buffer until the connection is freed */"
SHRINK_ORIGINAL = """    if (!forcedFree && (usedLength > STATIC_BUFFER_LEN ||
            ssl->buffers.clearOutputBuffer.length > 0))
        return;
"""
SHRINK_PATCHED = f"""    {SHRINK_MARKER}
#if defined(CROSSPOINT_WOLFSSL_RETAIN_INPUT_BUFFER)
    if (!forcedFree && ssl->options.handShakeState == HANDSHAKE_DONE)
        return;
#endif
{SHRINK_ORIGINAL}"""

GROW_MARKER = "/* CrossPoint: release a consumed input buffer before growing it */"
GROW_ORIGINAL = """    tmp = (byte*)XMALLOC(size + usedLength + align,
                             ssl->heap, DYNAMIC_TYPE_IN_BUFFER);
    WOLFSSL_MSG("growing input buffer");
"""
GROW_PATCHED = f"""    {GROW_MARKER}
#if defined(CROSSPOINT_WOLFSSL_RELEASE_EMPTY_INPUT_BUFFER)
    /* No unread ciphertext or pending plaintext may refer to this buffer.
     * ShrinkInputBuffer zeroes/frees it and restores valid static-buffer state,
     * including on allocation failure below. Records that fit never get here. */
    if (ssl->buffers.inputBuffer.dynamicFlag && usedLength == 0 &&
            ssl->buffers.inputBuffer.length == ssl->buffers.inputBuffer.idx &&
            ssl->buffers.clearOutputBuffer.length == 0) {{
        ShrinkInputBuffer(ssl, FORCED_FREE);
    }}
#endif
{GROW_ORIGINAL}"""


def _patch_once(path: Path, marker: str, original: str, patched: str, what: str) -> None:
    text = path.read_text()
    if marker in text:
        if text.count(patched) != 1:
            # An older revision of this script left a different patch body.
            print(f"WARNING: stale wolfSSL {what} patch in {path} -- delete .pio/libdeps/<env>/Arduino-wolfSSL and rebuild")
        return
    if text.count(original) != 1:
        # Refuse to guess: an unpatched build is stock wolfSSL (correct, just
        # one malloc per record again), a mis-applied patch is not.
        print(f"WARNING: wolfSSL {what} not patched, source differs from the pinned 5.7.2: {path}")
        return
    path.write_text(text.replace(original, patched, 1))
    print(f"Patched wolfSSL {what}: {path}")


def patch_input_buffer(path: Path) -> None:
    _patch_once(path, SHRINK_MARKER, SHRINK_ORIGINAL, SHRINK_PATCHED, "input buffer retention")
    _patch_once(path, GROW_MARKER, GROW_ORIGINAL, GROW_PATCHED, "input buffer growth")


# Only the env being built: the 4096 override is per env, and rewriting the
# other envs' copies on every build would make their next build recompile
# wolfSSL for nothing. Each env patches its own copy when it builds.
_env_settings = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"] / "Arduino-wolfSSL" / "src" / "user_settings.h"
if _env_settings.exists():
    patch_user_settings(_env_settings, wants_sp_4096())
    _env_internal = _env_settings.parent / "src" / "internal.c"
    if _env_internal.exists():
        patch_input_buffer(_env_internal)
    else:
        print(f"WARNING: wolfSSL internal.c not found for env {env['PIOENV']} — input buffer patch not applied")
else:
    print(f"WARNING: wolfSSL user_settings.h not found for env {env['PIOENV']} — TLS overrides not applied")

for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/wolfssl/wolfcrypt/settings.h"):
    patch_embos_rtos_include(settings)
