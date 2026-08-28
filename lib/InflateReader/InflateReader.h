#pragma once

#include <uzlib.h>

#include <cstddef>

// Return value for readAtMost().
enum class InflateStatus {
  Ok,     // Output buffer full; more compressed data remains.
  Done,   // Stream ended cleanly (TINF_DONE). produced may be < maxLen.
  Error,  // Decompression failed.
};

// Streaming deflate decompressor wrapping uzlib.
//
// Two modes:
//   init(false)  — one-shot: input is a contiguous buffer, call read() once.
//   init(true)   — streaming: allocates a 32KB ring buffer for back-references
//                  across multiple read() / readAtMost() calls.
//
// Streaming callback pattern:
//   The uzlib read callback receives a `struct uzlib_uncomp*` with no separate
//   context pointer. To attach context, make InflateReader the *first member* of
//   your context struct, then cast inside the callback:
//
//     struct MyCtx {
//       InflateReader reader;   // must be first
//       FsFile* file;
//       // ...
//     };
//     static int myCb(struct uzlib_uncomp* u) {
//       MyCtx* ctx = reinterpret_cast<MyCtx*>(u);   // valid: reader.decomp is at offset 0
//       // ... fill u->source / u->source_limit, return first byte
//     }
//     MyCtx ctx;
//     ctx.reader.init(true);
//     ctx.reader.setReadCallback(myCb);
//
// Size of the LZ77 back-reference window a streaming inflate needs. Fixed by the
// deflate format (32KB max window); a smaller dictionary silently corrupts any
// stream compressed with a larger one, so this is not tunable.
inline constexpr size_t INFLATE_DICT_BYTES = 32768;

// Lends InflateReader a caller-owned buffer to use as its dictionary instead of
// malloc'ing one.
//
// Why this exists: the dictionary is a single 32KB CONTIGUOUS allocation, and it
// is the allocation that fails first on this device. Measured on hardware, once
// the BLE stack has run once, ~19 small allocations survive its teardown scattered
// through the working heap region and cap the largest free block at ~25KB — for
// the rest of the session, with no way to recover it short of a reboot. Freeing
// more bytes cannot fix that; only using memory that was never fragmented can.
//
// The framebuffer is exactly that: allocated once at boot, permanently contiguous,
// and — because e-ink is bistable and holds its image without the buffer — idle for
// the whole of a section build. Lease it for the inflate, then clear and repaint.
//
// CALLER CONTRACT: nothing may render for the lifetime of the lease. The lease
// destroys the framebuffer's contents, so the caller must clear/repaint after.
//
// On PSRAM boards the lease is compiled out (active() always false, buffer left
// untouched) — the contiguity problem it works around does not exist there. See
// DevicePolicy.h.
class InflateScratchLease {
 public:
  InflateScratchLease(uint8_t* buffer, size_t length);
  ~InflateScratchLease();

  InflateScratchLease(const InflateScratchLease&) = delete;
  InflateScratchLease& operator=(const InflateScratchLease&) = delete;

  // False when the buffer was too small or another lease is already active; the
  // reader then falls back to malloc and the caller need not repaint.
  bool active() const { return registered; }

 private:
  bool registered = false;
};

// Borrow the region an InflateScratchLease is currently lending, when one is
// outstanding and nothing else is using it; returns nullptr otherwise.
//
// This exists for the content-protection read path, which inflates through
// miniz rather than uzlib and needs the same guaranteed-contiguous memory (one
// ~44KB allocation: the 32KB dictionary plus Huffman tables). The two never
// inflate simultaneously -- a single entry is read at a time -- so they share
// the one lent region instead of each leasing the framebuffer and overlapping.
uint8_t* borrowInflateScratch(size_t needed);

// Hand back a pointer from borrowInflateScratch(). Returns false if it did not
// come from the lent region, so callers can fall through to free().
bool returnInflateScratch(const uint8_t* buffer);

class InflateReader {
 public:
  InflateReader() = default;
  ~InflateReader();

  InflateReader(const InflateReader&) = delete;
  InflateReader& operator=(const InflateReader&) = delete;

  // Initialise decompressor. streaming=true allocates a 32KB ring buffer needed
  // when read() or readAtMost() will be called multiple times.
  // Returns false only in streaming mode if the ring buffer allocation fails.
  bool init(bool streaming = false);

  // Release the ring buffer and reset internal state.
  void deinit();

  // Set the entire compressed input as a contiguous memory buffer.
  // Used in one-shot mode; not needed when a read callback is set.
  void setSource(const uint8_t* src, size_t len);

  // Set a uzlib-compatible read callback for streaming input.
  // See class-level comment for the expected callback/context struct pattern.
  void setReadCallback(int (*cb)(uzlib_uncomp*));

  // Consume the 2-byte zlib header (CMF + FLG) from the input stream.
  // Call this once before the first read() when input is zlib-wrapped (e.g. PNG IDAT).
  void skipZlibHeader();

  // Decompress exactly len bytes into dest.
  // Returns false if the stream ends before producing len bytes, or on error.
  bool read(uint8_t* dest, size_t len);

  // Decompress up to maxLen bytes into dest.
  // Sets *produced to the number of bytes written.
  // Returns Done when the stream ends cleanly, Ok when there is more to read,
  // and Error on failure.
  InflateStatus readAtMost(uint8_t* dest, size_t maxLen, size_t* produced);

  // Returns a pointer to the underlying TINF_DATA.
  // Useful for advanced streaming setups where the callback needs access to the
  // uzlib struct directly (e.g. updating source/source_limit).
  uzlib_uncomp* raw() { return &decomp; }

 private:
  uzlib_uncomp decomp = {};
  uint8_t* ringBuffer = nullptr;
  // True when ringBuffer points at a leased buffer we must not free.
  bool ringBufferBorrowed = false;
};
