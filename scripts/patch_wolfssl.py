from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
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


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


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


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)

for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/wolfssl/wolfcrypt/settings.h"):
    patch_embos_rtos_include(settings)
