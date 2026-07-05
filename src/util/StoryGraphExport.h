#pragma once

#include <string>

// One-way export of the reading library to a CSV that StoryGraph can import.
//
// StoryGraph's importer ingests the Goodreads library-export format, so this
// writes that exact 24-column schema (not StoryGraph's own export schema). It
// is intentionally lossy: reading-time / streak / session analytics have no
// Goodreads column and are NOT included. Use the JSON export
// (ReadingStatsStore::exportToFile) for a lossless round-trip / backup.
//
// Rows are streamed to SD one at a time via HalStorage (no large in-RAM buffer).
namespace StoryGraphExport {

// Writes a StoryGraph CSV of every started/finished book to `path` (SD).
// Returns false if the file could not be opened for writing.
bool exportToCsv(const std::string& path);

}  // namespace StoryGraphExport
