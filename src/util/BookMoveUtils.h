#pragma once

#include <string>

// Renaming a file on the card. Everything this firmware keeps about a book is
// keyed by the book's path (cache directory, bookmarks, clippings and the sync
// sidecars are all named after a hash or a flattening of it; recents, pins,
// the shelved list and the reading stats store it verbatim), so a bare rename
// would orphan the book's progress. renameFile() carries that state across.
namespace BookMoveUtils {

enum class RenameResult {
  Ok,
  InvalidName,   // new name is empty, has a path separator or a FAT-forbidden character, or changes the book format
  TargetExists,  // something is already at the new path
  Failed,        // the card refused; the file still has its old name and its state is untouched
};

// Renames oldPath to newPath (both absolute, same folder or not). For a book
// (EPUB, XTC, TXT/Markdown) the reading state follows it:
//   - cache directory (progress, book.bin with tags, sections, cover thumbs)
//   - bookmarks and clippings (EPUB)
//   - BookFusion / Hardcover / KOReader sync sidecars
//   - recents entry, pin, shelved mark, reading stats, the remembered open book
// The cache directory moves before the file and is moved back if the file
// rename fails, so progress can never be stranded on a failed rename. The rest
// is carried over afterwards and a failure there is logged, not undone: the
// book is already under its new name at that point.
// Directories are refused (InvalidName): a folder rename would have to re-key
// every book below it.
RenameResult renameFile(const std::string& oldPath, const std::string& newPath);

}  // namespace BookMoveUtils
