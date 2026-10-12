#pragma once

#include <cstddef>
#include <cstdint>

// Flat "reading time per name" accumulator behind the Stats screen's top
// authors / top tags lists (LibraryReadingStats).
//
// Names are never stored. Each distinct name is one 16-byte bucket holding a
// hash of the name, its totals, and where one spelling of it can be read back
// from the Library index (record ordinal + position in that record's list), so
// only the handful of names that end up on screen are ever materialised. The
// caller owns the bucket storage; nothing here allocates, and the header is
// free of Arduino/HAL so it is host-testable (test/reading_key_totals).
namespace KeyTotals {

enum Kind : uint8_t { KIND_AUTHOR = 0, KIND_TAG = 1 };

struct Bucket {
  uint64_t key;      // makeKey(): name hash with the Kind in bit 0
  uint32_t seconds;  // reading time credited to this name
  uint16_t ordinal;  // an index record that carries the name...
  uint8_t books;     // books credited, saturating at 255
  uint8_t token;     // ...and the name's position in that record's list
};
static_assert(sizeof(Bucket) == 16, "Bucket must stay 16 bytes: the table is sized by it");

// One name out of a delimiter-separated list.
struct Token {
  const char* data = nullptr;
  size_t len = 0;
};

inline bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Next non-empty, trimmed token at or after `cursor`. Advances `cursor` past
// it; false when the list is exhausted. Tokens are counted by the caller in
// the order this returns them, which is what Bucket::token records.
inline bool nextToken(const char* text, const size_t len, const char delimiter, size_t& cursor, Token& out) {
  while (cursor <= len) {
    size_t end = cursor;
    while (end < len && text[end] != delimiter) end++;
    size_t first = cursor;
    size_t last = end;
    while (first < last && isSpace(text[first])) first++;
    while (last > first && isSpace(text[last - 1])) last--;
    cursor = end + 1;
    if (last > first) {
      out.data = text + first;
      out.len = last - first;
      return true;
    }
  }
  return false;
}

// FNV-1a 64 over the ASCII-lowercased bytes, so "Fantasy" and "fantasy" are one
// tag (the Tags/Authors browser compares the same way). Bit 0 carries the Kind,
// which keeps an author and a tag with the same spelling apart in one table.
inline uint64_t makeKey(const Kind kind, const char* text, const size_t len) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return (hash & ~1ULL) | static_cast<uint64_t>(kind);
}

inline Kind kindOf(const Bucket& bucket) { return static_cast<Kind>(bucket.key & 1ULL); }

class Table {
  Bucket* buckets;
  uint16_t capacity;
  uint16_t used = 0;
  uint16_t dropped = 0;

 public:
  Table(Bucket* storage, const uint16_t storageCount) : buckets(storage), capacity(storage ? storageCount : 0) {}

  uint16_t size() const { return used; }
  // add() calls that named a new key after the table filled. Totals for keys
  // already present stay exact; a dropped key is simply missing from the lists.
  uint16_t droppedCount() const { return dropped; }

  // Credit one book's reading time to `key`. A linear scan: the table holds at
  // most a few hundred buckets and is filled once per screen entry.
  void add(const uint64_t key, const uint32_t seconds, const uint16_t ordinal, const uint8_t token) {
    for (uint16_t i = 0; i < used; i++) {
      Bucket& bucket = buckets[i];
      if (bucket.key != key) continue;
      bucket.seconds = (seconds > UINT32_MAX - bucket.seconds) ? UINT32_MAX : bucket.seconds + seconds;
      if (bucket.books < UINT8_MAX) bucket.books++;
      return;
    }
    if (used >= capacity) {
      if (dropped < UINT16_MAX) dropped++;
      return;
    }
    buckets[used++] = Bucket{key, seconds, ordinal, 1, token};
  }

  // Copy the `maxOut` buckets of `kind` with the most reading time into `out`,
  // best first. Ties go to the name with more books, then to the one seen
  // first, so the order is stable from one visit to the next.
  uint8_t top(const Kind kind, Bucket* out, const uint8_t maxOut) const {
    uint8_t count = 0;
    for (uint16_t i = 0; i < used; i++) {
      const Bucket& candidate = buckets[i];
      if (kindOf(candidate) != kind) continue;
      uint8_t pos = count;
      while (pos > 0 && better(candidate, out[pos - 1])) pos--;
      if (pos >= maxOut) continue;
      const uint8_t last = count < maxOut ? count : static_cast<uint8_t>(maxOut - 1);
      for (uint8_t j = last; j > pos; j--) out[j] = out[j - 1];
      out[pos] = candidate;
      if (count < maxOut) count++;
    }
    return count;
  }

 private:
  static bool better(const Bucket& a, const Bucket& b) {
    if (a.seconds != b.seconds) return a.seconds > b.seconds;
    return a.books > b.books;
  }
};

}  // namespace KeyTotals
