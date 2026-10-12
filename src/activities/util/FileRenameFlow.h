#pragma once

#include <functional>
#include <string>

class Activity;
class GfxRenderer;
class MappedInputManager;

// The rename flow every screen with a Rename action shares: a keyboard
// pre-filled with the file's name minus its extension, then
// BookMoveUtils::renameFile so a book keeps its reading state, then a dialog if
// the new name was refused.
//
// A host keeps one of these as a member, calls start() from its Rename action
// and poll() at the top of its loop(). What the host does once the file has its
// new name (reload its list, move its cursor) is the onRenamed callback.
class FileRenameFlow {
 public:
  using RenamedFn = std::function<void(const std::string& oldPath, const std::string& newPath)>;

  // Opens the keyboard over `host`. Only the part before the extension is
  // edited, so a rename can never change what kind of file this is (and with it
  // which reader and cache it uses). onRenamed runs only when the file was
  // actually renamed; a cancel, an unchanged name or a refusal does not call it.
  void start(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& path,
             RenamedFn onRenamed);

  // Call first thing in the host's loop(). True means this frame belongs to the
  // flow and the host must return: either the Back that cancelled the keyboard
  // is still on its way up (the keyboard cancels on the press, so its release
  // would otherwise reach the host as "leave this screen"), or the "rename
  // failed" dialog has just been opened.
  bool poll(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput);

 private:
  void rename(const std::string& oldPath, const std::string& newStem, const std::string& extension,
              const RenamedFn& onRenamed);

  // Why the last rename was refused (a tr() string; "" when the dialog heading
  // says it all). Set from the keyboard's result handler, shown by poll().
  const char* pendingError = nullptr;
  bool awaitingBackRelease = false;
};
