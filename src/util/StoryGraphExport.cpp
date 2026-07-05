#include "StoryGraphExport.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <string>

#include "ReadingStatsStore.h"
#include "util/TimeUtils.h"

namespace StoryGraphExport {
namespace {

constexpr const char* MODULE = "STORYGRAPH";

// StoryGraph's importer ingests the Goodreads library-export CSV, so we emit
// that exact 24-column header (in order — StoryGraph rejects reordered or
// missing columns). We only populate the columns we track; the rest are empty.
constexpr const char* CSV_HEADER =
    "Book Id,Title,Author,Author l-f,Additional Authors,ISBN,ISBN13,My Rating,Average Rating,Publisher,Binding,"
    "Number of Pages,Year Published,Original Publication Year,Date Read,Date Added,Bookshelves,"
    "Bookshelves with positions,Exclusive Shelf,My Review,Spoiler,Private Notes,Read Count,Owned Copies\n";

// RFC-4180 field quoting: wrap in double quotes and double any embedded quote
// when the field contains a comma, quote, or newline.
std::string escapeField(const std::string& field) {
  if (field.find_first_of(",\"\n\r") == std::string::npos) {
    return field;
  }
  std::string out;
  out.reserve(field.size() + 2);
  out.push_back('"');
  for (const char c : field) {
    if (c == '"') {
      out.push_back('"');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

// StoryGraph expects dates as YYYY/MM/DD. TimeUtils::formatDate emits
// DD/MM/YYYY, so we format from calendar parts directly here.
std::string formatCsvDate(const uint32_t epochSeconds) {
  if (!TimeUtils::isClockValid(epochSeconds)) {
    return "";
  }
  const uint32_t dayOrdinal = TimeUtils::getLocalDayOrdinal(epochSeconds);
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (!TimeUtils::getDateFromDayOrdinal(dayOrdinal, year, month, day)) {
    return "";
  }
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%04d/%02u/%02u", year, month, day);
  return buffer;
}

void writeStr(HalFile& file, const std::string& text) {
  if (!text.empty()) {
    file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  }
}

}  // namespace

bool exportToCsv(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForWrite(MODULE, path.c_str(), file)) {
    LOG_ERR(MODULE, "Failed to open %s for write", path.c_str());
    return false;
  }

  writeStr(file, CSV_HEADER);

  uint32_t rows = 0;
  for (const auto& book : READING_STATS.getBooks()) {
    const std::string title = book.title.empty() ? book.path : book.title;
    const std::string dateAdded = formatCsvDate(book.firstReadAt);
    // Date Read is only meaningful for finished books (their completion date).
    const std::string dateRead =
        book.completed ? formatCsvDate(book.completedAt != 0 ? book.completedAt : book.lastReadAt) : "";

    // Goodreads column order (see CSV_HEADER). Empty fields are intentional —
    // we don't track ISBN, rating, publisher, pages, reviews, etc. StoryGraph
    // keys off Title/Author, Exclusive Shelf, the dates, and Read Count.
    const std::string fields[24] = {
        "",                                             // Book Id
        escapeField(title),                             // Title
        escapeField(book.author),                       // Author
        "",                                             // Author l-f
        "",                                             // Additional Authors
        "",                                             // ISBN
        "",                                             // ISBN13
        "0",                                            // My Rating (0 = unrated)
        "",                                             // Average Rating
        "",                                             // Publisher
        "",                                             // Binding
        "",                                             // Number of Pages
        "",                                             // Year Published
        "",                                             // Original Publication Year
        dateRead,                                       // Date Read
        dateAdded,                                      // Date Added
        "",                                             // Bookshelves
        "",                                             // Bookshelves with positions
        book.completed ? "read" : "currently-reading",  // Exclusive Shelf
        "",                                             // My Review
        "",                                             // Spoiler
        "",                                             // Private Notes
        book.completed ? "1" : "0",                     // Read Count
        "0",                                            // Owned Copies
    };

    std::string line;
    line.reserve(160);
    for (int i = 0; i < 24; ++i) {
      if (i != 0) {
        line += ',';
      }
      line += fields[i];
    }
    line += '\n';
    writeStr(file, line);
    rows++;
  }

  file.close();
  LOG_INF(MODULE, "Exported %u books to %s", rows, path.c_str());
  return true;
}

}  // namespace StoryGraphExport
