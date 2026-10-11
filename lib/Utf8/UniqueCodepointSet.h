#pragma once

#include <algorithm>
#include <cstdint>

// Collects unique codepoints into a caller-owned buffer, with no heap use.
//
// Replaces "compare every character against every codepoint seen so far"
// collectors, which cost ~50 compares per Latin character and 1,000+ per CJK
// character once a chapter has that many unique glyphs. ASCII lives in a
// 128-bit bitmap, so most Latin text never touches the buffer. Other
// codepoints are binary-searched in a sorted prefix, with a short unsorted tail
// (at most TAIL_MAX entries, scanned linearly) that is folded into the prefix
// by a sort when it fills. Membership is exact at all times, so the capacity
// check is too.
//
// The object itself is ~40 bytes and is meant to live on the stack.
class UniqueCodepointSet {
 public:
  // `buffer` must hold `capacity` entries and outlive the set.
  UniqueCodepointSet(uint32_t* buffer, const uint32_t capacity) : buf_(buffer), cap_(capacity) {}

  // Adds `cp`. Returns false only when `cp` is new and the set is already at
  // capacity (the caller's "cap hit"); true if it was added or already present.
  // Zero is ignored and reports true.
  bool add(const uint32_t cp) {
    if (cp == 0) return true;
    if (cp < ASCII_LIMIT) {
      const uint32_t bit = 1u << (cp & 31u);
      uint32_t& word = ascii_[cp >> 5];
      if (word & bit) return true;
      if (size() >= cap_) return false;
      word |= bit;
      ++asciiCount_;
      return true;
    }
    if (std::binary_search(buf_, buf_ + sorted_, cp)) return true;
    for (uint32_t i = sorted_; i < count_; ++i) {
      if (buf_[i] == cp) return true;
    }
    if (size() >= cap_) return false;
    buf_[count_++] = cp;
    if (count_ - sorted_ >= TAIL_MAX) foldTail();
    return true;
  }

  // Number of unique codepoints collected so far.
  uint32_t size() const { return count_ + asciiCount_; }

  // Leaves every collected codepoint in buffer[0..n) in ascending order and
  // returns n. Call once, after the last add(); the set must not be used again.
  uint32_t finish() {
    foldTail();
    // Buffered codepoints are all >= ASCII_LIMIT, so append the ASCII block in
    // order and rotate it to the front. size() <= cap_ guarantees it fits.
    const uint32_t nonAscii = count_;
    for (uint32_t cp = 1; cp < ASCII_LIMIT; ++cp) {
      if (ascii_[cp >> 5] & (1u << (cp & 31u))) buf_[count_++] = cp;
    }
    std::rotate(buf_, buf_ + nonAscii, buf_ + count_);
    asciiCount_ = 0;
    sorted_ = count_;
    return count_;
  }

 private:
  static constexpr uint32_t ASCII_LIMIT = 128;
  static constexpr uint32_t TAIL_MAX = 32;

  // The tail is unique and disjoint from the prefix (add() checks both), so a
  // plain sort of the whole buffer restores a sorted, unique prefix. std::sort
  // rather than std::inplace_merge: the latter may allocate a temporary buffer.
  void foldTail() {
    if (sorted_ == count_) return;
    std::sort(buf_, buf_ + count_);
    sorted_ = count_;
  }

  uint32_t* buf_;
  uint32_t cap_;
  uint32_t count_ = 0;   // non-ASCII entries in buf_ (sorted prefix + unsorted tail)
  uint32_t sorted_ = 0;  // length of the sorted prefix
  uint32_t asciiCount_ = 0;
  uint32_t ascii_[ASCII_LIMIT / 32] = {};
};
