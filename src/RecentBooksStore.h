#pragma once
#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  int8_t progressPercent = -1;  // -1 = unknown, 0-100 = percentage read
  bool pinned = false;          // set by HomeActivity on the shelf entries it took from the pinned list

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore;
namespace JsonSettingsIO {
bool loadRecentBooks(RecentBooksStore& store, const char* json);
}  // namespace JsonSettingsIO

class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  std::vector<RecentBook> recentBooks;
  // Books the user put away with "Shelve Book". The Bookshelf home fills its
  // empty slots from the library, and without this list a shelved book that
  // happens to sort first would be picked straight back. Bounded (oldest out);
  // opening a book takes it off again.
  std::vector<std::string> shelvedPaths;
  // Books pinned to the Bookshelf home: they take the first shelf slots, in pin
  // order, whatever has been opened since. Shelving or deleting a book unpins it.
  std::vector<std::string> pinnedPaths;
  // The list is read from the card the first time anything asks for it and can be released
  // again (release()) while a screen that never shows it is up -- the reader, for the whole of
  // a session. Keeping it resident from boot cost ~4.5 KB and split the largest free block
  // before a book was even opened (witchhunt-reader's measurement; same store shape here).
  // Every mutation saves at once, so releasing never loses anything.
  mutable bool loaded_ = false;
  void ensureLoaded() const;

 public:
  static constexpr size_t MAX_SHELVED_BOOKS = 16;
  static constexpr size_t MAX_PINNED_BOOKS = 8;

 private:
  friend bool JsonSettingsIO::loadRecentBooks(RecentBooksStore&, const char*);

 public:
  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);
  void updateProgress(const std::string& path, int8_t progressPercent);
  void removeBook(const std::string& path);
  // A book file was renamed or moved: carry its recents entry (progress
  // included), its shelved mark and its pin over to the new path. The cover
  // thumbnail path lives inside the book's cache directory, so its
  // oldCachePath prefix is swapped for newCachePath. Persists only when
  // something changed; returns false only if that save failed.
  bool updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  // "Shelve Book": drop the entry and remember the path so library backfill
  // skips it. Persists.
  void shelveBook(const std::string& path);
  bool isShelved(const std::string& path) const;
  const std::vector<std::string>& getShelvedPaths() const {
    ensureLoaded();
    return shelvedPaths;
  }

  // Pin / unpin for the Bookshelf home. pinBook also takes the path off the
  // shelved list. Both persist. togglePin returns the new state.
  void pinBook(const std::string& path);
  void unpinBook(const std::string& path);
  bool togglePin(const std::string& path);
  bool isPinned(const std::string& path) const;
  const std::vector<std::string>& getPinnedPaths() const {
    ensureLoaded();
    return pinnedPaths;
  }

  // True if the book's backing file is no longer present on the SD card.
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist — caller decides.
  bool pruneMissing();

  // Get the list of recent books (most recent first). The reference is valid until release().
  const std::vector<RecentBook>& getBooks() const {
    ensureLoaded();
    return recentBooks;
  }

  // Get the count of recent books
  int getCount() const {
    ensureLoaded();
    return static_cast<int>(recentBooks.size());
  }
  // Frees the list; the next access reads it again. For screens that do not show it.
  void release();

  bool saveToFile() const;

  bool loadFromFile();
  RecentBook getDataFromBook(std::string path) const;

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
