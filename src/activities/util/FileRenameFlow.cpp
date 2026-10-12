#include "FileRenameFlow.h"

#include <I18n.h>

#include <memory>
#include <variant>

#include "ConfirmationActivity.h"
#include "KeyboardEntryActivity.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/BookMoveUtils.h"

namespace {
// FAT long names hold 255 UTF-16 units; counting bytes keeps any UTF-8 name inside that.
constexpr size_t MAX_FILE_NAME_BYTES = 255;
}  // namespace

void FileRenameFlow::start(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput,
                           const std::string& path, RenamedFn onRenamed) {
  const auto slash = path.find_last_of('/');
  const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const auto dot = name.find_last_of('.');
  const bool hasExtension = dot != std::string::npos && dot > 0;
  const std::string extension = hasExtension ? name.substr(dot) : std::string();
  const std::string stem = hasExtension ? name.substr(0, dot) : name;
  if (extension.size() >= MAX_FILE_NAME_BYTES) return;

  host.startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME), stem,
                                              MAX_FILE_NAME_BYTES - extension.size(), InputType::Text),
      [this, &host, &mappedInput, path, extension, onRenamed = std::move(onRenamed)](const ActivityResult& result) {
        awaitingBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
        const auto* typed = std::get_if<KeyboardResult>(&result.data);
        if (!result.isCancelled && typed) rename(path, typed->text, extension, onRenamed);
        host.requestUpdate();
      });
}

void FileRenameFlow::rename(const std::string& oldPath, const std::string& newStem, const std::string& extension,
                            const RenamedFn& onRenamed) {
  const auto slash = oldPath.find_last_of('/');
  const std::string newPath = oldPath.substr(0, slash + 1) + newStem + extension;
  if (newPath == oldPath) return;

  switch (BookMoveUtils::renameFile(oldPath, newPath)) {
    case BookMoveUtils::RenameResult::Ok:
      if (onRenamed) onRenamed(oldPath, newPath);
      return;
    case BookMoveUtils::RenameResult::InvalidName:
      pendingError = tr(STR_RENAME_INVALID_NAME);
      return;
    case BookMoveUtils::RenameResult::TargetExists:
      pendingError = tr(STR_RENAME_NAME_EXISTS);
      return;
    case BookMoveUtils::RenameResult::Failed:
      pendingError = "";
      return;
  }
}

bool FileRenameFlow::poll(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput) {
  if (pendingError) {
    const char* reason = pendingError;
    pendingError = nullptr;
    awaitingBackRelease = false;  // the dialog is on top by the time it comes up
    host.startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_RENAME_FAILED), reason),
        [&host](const ActivityResult&) { host.requestUpdate(); });
    return true;
  }
  if (awaitingBackRelease) {
    // Held, or released on this very frame: either way the host must not see it.
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      awaitingBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
      return true;
    }
    awaitingBackRelease = false;
  }
  return false;
}
