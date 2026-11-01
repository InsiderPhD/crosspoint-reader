#pragma once

#include <string>

// Moves a book into the user's finished-books folder (SETTINGS.finishedBookFolder)
// once a reading session has finished it, carrying its per-book state across to
// the new path: render cache, recents, reading stats, sync sidecars and
// APP_STATE. Without that, the path-hashed cache and sidecars would be orphaned.
namespace FinishedBookMover {

// Call at the end of a reader's onExit(): after READING_STATS.endSession() (which
// decides whether this session finished the book) and after the book object has
// been released, so no file handle is open on it during the rename. No-op unless
// the setting is on and the session that just ended finished the book.
// Returns true if the book was moved.
bool moveIfFinished(const std::string& bookPath);

}  // namespace FinishedBookMover
