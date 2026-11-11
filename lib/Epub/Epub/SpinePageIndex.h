#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Per-spine page counts for one layout variant, in "<book cache>/pages.bin".
//
// The reader's book-wide page total (time left, "page X of Y") used to be summed by opening
// every spine's section cache file to read its header: one FAT directory walk per spine item,
// which on a 1,700-file book was the slowest part of a cached open. Every finished section
// build (and every cache hit) now records its page count here, and the total is one small
// sequential read. The shape of witchhunt-reader's SpinePageIndex; the file is ours.
//
// Layout: magic, version, layoutKey (Section::layoutKey of the render settings the counts were
// laid out under), spineCount, then one uint16_t per spine (0 = not laid out yet). A key or
// count that does not match the request is treated as no table: the counts belong to another
// font size, margin or orientation.
namespace SpinePageIndex {
constexpr uint32_t MAGIC = 0x50504BFF;  // bytes: 0xFF, "KPP"
constexpr uint8_t VERSION = 1;

// Reads the table for `layoutKey`. False (and `counts` empty) when there is none for this key.
bool read(const std::string& bookCachePath, uint32_t layoutKey, uint16_t spineCount, std::vector<uint16_t>& counts);
// Writes the whole table for `layoutKey`.
bool writeAll(const std::string& bookCachePath, uint32_t layoutKey, const std::vector<uint16_t>& counts);
// Records one spine's count: a no-op when the table already says so, a rewrite of the table
// otherwise (a few KB at most). A table for another key is replaced by one holding this
// entry alone.
bool record(const std::string& bookCachePath, uint32_t layoutKey, uint16_t spineCount, uint16_t spineIndex,
            uint16_t pages);
}  // namespace SpinePageIndex
