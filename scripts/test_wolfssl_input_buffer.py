"""Host check for the wolfSSL input-buffer patches in scripts/patch_wolfssl.py.

Extracts ShrinkInputBuffer/GrowInputBuffer from the pinned Arduino-wolfSSL
internal.c, applies the patches, and runs them against an allocator with a
byte budget. No network or device is needed.

    python3 scripts/test_wolfssl_input_buffer.py

Set WOLFSSL_INTERNAL_SOURCE to an unpatched or patched src/src/internal.c when
no .pio/libdeps/<env>/Arduino-wolfSSL exists in this checkout.
"""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte;
typedef uint32_t word32;
#define STATIC_BUFFER_LEN 5
#define DTLS_RECORD_HEADER_SZ 13
#define WOLFSSL_GENERAL_ALIGNMENT 0
#define MEMORY_E -125
#define BAD_FUNC_ARG -173
#define FORCED_FREE 1
#define HANDSHAKE_DONE 16
#define DYNAMIC_TYPE_IN_BUFFER 0
#define WOLFSSL_MSG(x) ((void)0)
#define XMEMCPY memcpy
#define ForceZero(p, n) memset(p, 0, n)
#define IsEncryptionOn(s, r) 1
#define XMALLOC(n, h, t) alloc_buffer(n)
#define XFREE(p, h, t) free_buffer(p)
typedef union { size_t size; max_align_t alignment; } Header;
static size_t live, peak, budget, allocs;
static void *alloc_buffer(size_t size) {
    if (size > budget - live) return NULL;
    Header *h = malloc(sizeof(Header) + size);
    assert(h);
    h->size = size;
    live += size;
    allocs++;
    if (live > peak) peak = live;
    return h + 1;
}
static void free_buffer(void *ptr) {
    Header *h = (Header *)ptr - 1;
    assert(live >= h->size);
    live -= h->size;
    free(h);
}
typedef struct {
    byte *buffer;
    byte staticBuffer[STATIC_BUFFER_LEN];
    word32 bufferSize, length, idx;
    byte dynamicFlag, offset;
} Buffer;
typedef struct {
    struct { Buffer inputBuffer, clearOutputBuffer; } buffers;
    struct { int dtls; int handShakeState; } options;
    void *heap;
} WOLFSSL;
'''

CHECKS = r'''
static void init(WOLFSSL *ssl, int handshakeDone) {
    memset(ssl, 0, sizeof(*ssl));
    ssl->buffers.inputBuffer.bufferSize = STATIC_BUFFER_LEN;
    ssl->buffers.inputBuffer.buffer = ssl->buffers.inputBuffer.staticBuffer;
    ssl->options.handShakeState = handshakeDone ? HANDSHAKE_DONE : 0;
    budget = 20000;
    peak = 0;
    allocs = 0;
    assert(live == 0);
}
/* What SSL_ResourceFree does on wolfSSL_free. */
static void release(WOLFSSL *ssl) {
    if (ssl->buffers.inputBuffer.dynamicFlag) ShrinkInputBuffer(ssl, FORCED_FREE);
}
/* One record: grow to fit, mark it fully consumed, then the post-record shrink. */
static int record(WOLFSSL *ssl, int size) {
    if ((word32)size > ssl->buffers.inputBuffer.bufferSize) {
        int ret = GrowInputBuffer(ssl, size, 0);
        if (ret != 0) return ret;
    }
    memset(ssl->buffers.inputBuffer.buffer, 0x5a, (size_t)size);
    ssl->buffers.inputBuffer.idx = ssl->buffers.inputBuffer.length = (word32)size;
    if (ssl->buffers.inputBuffer.dynamicFlag) ShrinkInputBuffer(ssl, 0);
    return 0;
}
int main(void) {
    WOLFSSL ssl;

    /* During the handshake the stock shrink still runs: nothing is retained. */
    init(&ssl, 0);
    assert(record(&ssl, 6000) == 0);
    assert(ssl.buffers.inputBuffer.dynamicFlag == 0 && live == 0);

    /* After the handshake a buffer is reused by every record that fits. */
    init(&ssl, 1);
    for (int i = 0; i < 50; i++) assert(record(&ssl, 16416) == 0);
#ifdef EXPECT_STOCK
    assert(allocs == 50 && live == 0);
#else
    assert(allocs == 1 && live == 16416);
    assert(record(&ssl, 300) == 0 && allocs == 1); /* smaller record: no realloc */
#endif
    assert(peak == 16416);
    release(&ssl); /* wolfSSL_free: nothing outlives the session */
    assert(live == 0 && ssl.buffers.inputBuffer.dynamicFlag == 0);

    /* A larger record after a smaller one must not need old + new at once. */
    init(&ssl, 1);
    assert(record(&ssl, 8192) == 0);
    assert(record(&ssl, 16416) == 0);
    assert(peak == 16416);
    release(&ssl);
    assert(live == 0);

#ifndef EXPECT_STOCK
    /* Failed growth leaves a valid, empty static buffer and supports a retry. */
    init(&ssl, 1);
    assert(record(&ssl, 8192) == 0);
    budget = live;
    assert(GrowInputBuffer(&ssl, 16416, 0) == MEMORY_E);
    assert(live == 0 && ssl.buffers.inputBuffer.dynamicFlag == 0);
    assert(ssl.buffers.inputBuffer.buffer == ssl.buffers.inputBuffer.staticBuffer);
    assert(ssl.buffers.inputBuffer.idx == 0 && ssl.buffers.inputBuffer.length == 0);
    budget = 20000;
    assert(GrowInputBuffer(&ssl, 16416, 0) == 0);
    release(&ssl);
    assert(live == 0);

    /* Unread ciphertext must survive both failed and successful growth. */
    init(&ssl, 1);
    assert(record(&ssl, 8192) == 0);
    byte *old = ssl.buffers.inputBuffer.buffer;
    ssl.buffers.inputBuffer.idx = 17;
    ssl.buffers.inputBuffer.length = 20;
    memcpy(old + 17, "abc", 3);
    assert(GrowInputBuffer(&ssl, 16416, 3) == MEMORY_E);
    assert(ssl.buffers.inputBuffer.buffer == old && memcmp(old + 17, "abc", 3) == 0);
    budget = 40000;
    assert(GrowInputBuffer(&ssl, 16416, 3) == 0);
    assert(ssl.buffers.inputBuffer.idx == 0 && ssl.buffers.inputBuffer.length == 3);
    assert(memcmp(ssl.buffers.inputBuffer.buffer, "abc", 3) == 0);
    release(&ssl);
    assert(live == 0);

    /* Pending plaintext can still point into a consumed ciphertext buffer. */
    init(&ssl, 1);
    assert(record(&ssl, 8192) == 0);
    old = ssl.buffers.inputBuffer.buffer;
    ssl.buffers.clearOutputBuffer.length = 1;
    assert(GrowInputBuffer(&ssl, 16416, 0) == MEMORY_E);
    assert(ssl.buffers.inputBuffer.buffer == old);
    ssl.buffers.clearOutputBuffer.length = 0;
    release(&ssl);
    assert(live == 0);
#endif
    return 0;
}
'''


class InputBufferPatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        root = self.root

        class Env(dict):
            def subst(self, value):
                return str(root / "libdeps") if "LIBDEPS" in value else str(root)

            def GetProjectOption(self, *_args):
                return ""

        env = Env(PIOENV="host")
        self.module = runpy.run_path(
            str(ROOT / "scripts/patch_wolfssl.py"), init_globals={"Import": lambda _: None, "env": env}
        )

    def test_idempotent_and_leaves_unknown_source_alone(self):
        path = self.root / "internal.c"
        path.write_text(self.module["SHRINK_ORIGINAL"] + self.module["GROW_ORIGINAL"])
        patch = self.module["patch_input_buffer"]
        patch(path)
        once = path.read_text()
        self.assertIn(self.module["SHRINK_MARKER"], once)
        self.assertIn(self.module["GROW_MARKER"], once)
        patch(path)
        self.assertEqual(once, path.read_text())
        path.write_text("unknown source")
        patch(path)
        self.assertEqual(path.read_text(), "unknown source")

    def test_patched_functions_with_limited_heap(self):
        source = os.environ.get("WOLFSSL_INTERNAL_SOURCE")
        candidates = sorted(ROOT.glob(".pio/libdeps/*/Arduino-wolfSSL/src/src/internal.c"))
        if not source and not candidates:
            self.skipTest("Install Arduino-wolfSSL or set WOLFSSL_INTERNAL_SOURCE")
        path = self.root / "internal.c"
        path.write_text(Path(source or candidates[0]).read_text())
        self.module["patch_input_buffer"](path)
        text = path.read_text()
        self.assertIn(self.module["SHRINK_PATCHED"], text)
        self.assertIn(self.module["GROW_PATCHED"], text)
        functions = ""
        for signature in ("void ShrinkInputBuffer(", "int GrowInputBuffer("):
            start = text.index(signature)
            end = text.index("\n}\n", start) + 3
            functions += text[start:end]
        for stock in (True, False):
            with self.subTest(stock=stock):
                cfile = self.root / "check.c"
                binary = self.root / "check"
                cfile.write_text(HARNESS + functions + CHECKS)
                flags = (
                    ["-DEXPECT_STOCK"]
                    if stock
                    else [
                        "-DCROSSPOINT_WOLFSSL_RETAIN_INPUT_BUFFER",
                        "-DCROSSPOINT_WOLFSSL_RELEASE_EMPTY_INPUT_BUFFER",
                    ]
                )
                subprocess.run(
                    ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", *flags,
                     str(cfile), "-o", str(binary)],
                    check=True,
                )
                subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
