#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>
#include <Xtc.h>

#include <algorithm>

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE_BIN[] = "/.crosspoint/recent.bin";
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.crosspoint/recent.json";
constexpr char RECENT_BOOKS_FILE_BAK[] = "/.crosspoint/recent.bin.bak";
constexpr int MAX_RECENT_BOOKS = 10;
}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  // Re-opening the book that is already first with identical details (sleep-wake
  // resume, Continue Reading) changes nothing: skip both the per-entry exists()
  // probes below and the full JSON rewrite. Stale entries can wait for the next
  // real add; HomeActivity already hides missing books when it lists them.
  if (!recentBooks.empty()) {
    const RecentBook& front = recentBooks.front();
    if (front.path == path && front.title == title && front.author == author && front.coverBmpPath == coverBmpPath) {
      return;
    }
  }

  // Drop stale entries first so a new add can't evict a valid book in their stead.
  pruneMissing();

  // Reading a shelved book puts it back in circulation.
  shelvedPaths.erase(std::remove(shelvedPaths.begin(), shelvedPaths.end(), path), shelvedPaths.end());

  // Preserve existing progress percentage if the book was already in the list
  int8_t existingProgress = -1;
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    existingProgress = it->progressPercent;
    recentBooks.erase(it);
  }

  // Add to front, carrying over the progress percentage
  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath, existingProgress});

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    if (book.title == title && book.author == author && book.coverBmpPath == coverBmpPath) return;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    saveToFile();
  }
}

void RecentBooksStore::removeBook(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  const bool wasPinned = isPinned(path);
  if (wasPinned) pinnedPaths.erase(std::remove(pinnedPaths.begin(), pinnedPaths.end(), path), pinnedPaths.end());
  if (it != recentBooks.end()) recentBooks.erase(it);
  if (it != recentBooks.end() || wasPinned) saveToFile();
}

void RecentBooksStore::shelveBook(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) recentBooks.erase(it);
  pinnedPaths.erase(std::remove(pinnedPaths.begin(), pinnedPaths.end(), path), pinnedPaths.end());
  if (!isShelved(path)) {
    if (shelvedPaths.size() >= RecentBooksStore::MAX_SHELVED_BOOKS) shelvedPaths.erase(shelvedPaths.begin());
    shelvedPaths.push_back(path);
  }
  saveToFile();
}

bool RecentBooksStore::isShelved(const std::string& path) const {
  return std::find(shelvedPaths.begin(), shelvedPaths.end(), path) != shelvedPaths.end();
}

void RecentBooksStore::pinBook(const std::string& path) {
  if (isPinned(path)) return;
  if (pinnedPaths.size() >= MAX_PINNED_BOOKS) pinnedPaths.erase(pinnedPaths.begin());
  pinnedPaths.push_back(path);
  shelvedPaths.erase(std::remove(shelvedPaths.begin(), shelvedPaths.end(), path), shelvedPaths.end());
  saveToFile();
}

void RecentBooksStore::unpinBook(const std::string& path) {
  const size_t before = pinnedPaths.size();
  pinnedPaths.erase(std::remove(pinnedPaths.begin(), pinnedPaths.end(), path), pinnedPaths.end());
  if (pinnedPaths.size() != before) saveToFile();
}

bool RecentBooksStore::togglePin(const std::string& path) {
  if (isPinned(path)) {
    unpinBook(path);
    return false;
  }
  pinBook(path);
  return true;
}

bool RecentBooksStore::isPinned(const std::string& path) const {
  return std::find(pinnedPaths.begin(), pinnedPaths.end(), path) != pinnedPaths.end();
}

void RecentBooksStore::updateProgress(const std::string& path, int8_t progressPercent) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end() && it->progressPercent != progressPercent) {
    it->progressPercent = progressPercent;
    saveToFile();
  }
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}

bool RecentBooksStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveRecentBooks(*this, RECENT_BOOKS_FILE_JSON);
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}

bool RecentBooksStore::loadFromFile() {
  // Try JSON first
  if (Storage.exists(RECENT_BOOKS_FILE_JSON)) {
    String json = Storage.readFile(RECENT_BOOKS_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadRecentBooks(*this, json.c_str());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(RECENT_BOOKS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      saveToFile();
      Storage.rename(RECENT_BOOKS_FILE_BIN, RECENT_BOOKS_FILE_BAK);
      LOG_DBG("RBS", "Migrated recent.bin to recent.json");
      return true;
    }
  }

  return false;
}

bool RecentBooksStore::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("RBS", RECENT_BOOKS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version == 1 || version == 2) {
    // Old version, just read paths
    uint8_t count;
    serialization::readPod(inputFile, count);
    recentBooks.clear();
    recentBooks.reserve(count);
    for (uint8_t i = 0; i < count; i++) {
      std::string path;
      serialization::readString(inputFile, path);

      // load book to get missing data
      RecentBook book = getDataFromBook(path);
      if (book.title.empty() && book.author.empty() && version == 2) {
        // Fall back to loading what we can from the store
        std::string title, author;
        serialization::readString(inputFile, title);
        serialization::readString(inputFile, author);
        recentBooks.push_back({path, title, author, ""});
      } else {
        recentBooks.push_back(book);
      }
    }
  } else if (version == 3) {
    uint8_t count;
    serialization::readPod(inputFile, count);

    recentBooks.clear();
    recentBooks.reserve(count);
    uint8_t omitted = 0;

    for (uint8_t i = 0; i < count; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      serialization::readString(inputFile, title);
      serialization::readString(inputFile, author);
      serialization::readString(inputFile, coverBmpPath);

      // Omit books with missing title (e.g. saved before metadata was available)
      if (title.empty()) {
        omitted++;
        continue;
      }

      recentBooks.push_back({path, title, author, coverBmpPath});
    }

    if (omitted > 0) {
      // Explicitly close() file before saveToFile() rewrites the same file
      inputFile.close();
      saveToFile();
      LOG_DBG("RBS", "Omitted %u recent book(s) with missing title", omitted);
      return true;
    }
  } else {
    LOG_ERR("RBS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  LOG_DBG("RBS", "Recent books loaded from binary file (%d entries)", static_cast<int>(recentBooks.size()));
  return true;
}
