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


# Only the env being built: the 4096 override is per env, and rewriting the
# other envs' copies on every build would make their next build recompile
# wolfSSL for nothing. Each env patches its own copy when it builds.
_env_settings = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"] / "Arduino-wolfSSL" / "src" / "user_settings.h"
if _env_settings.exists():
    patch_user_settings(_env_settings, wants_sp_4096())
else:
    print(f"WARNING: wolfSSL user_settings.h not found for env {env['PIOENV']} — TLS overrides not applied")

for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/wolfssl/wolfcrypt/settings.h"):
    patch_embos_rtos_include(settings)
