#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "../Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;
class Txt;

// Gates "this is the book to come back to" on the book having put a page on the
// panel (upstream a70ade72e). APP_STATE.openEpubPath and the recents entry used
// to be written in each reader's onEnter(), before the book was proven
// renderable, so a book that fails to index was still the one the device woke
// into. Now ReaderActivity::onEnter() forgets the previous book when an open
// starts, and each reader remembers its own only once a page has rendered.
//
// One per reader activity. markPageRendered() is the render task's half;
// takeFirstRender() and the statics belong to the main task.
class RenderedBookGate {
  std::atomic<bool> pageRendered{false};
  bool remembered = false;

 public:
  void markPageRendered() { pageRendered.store(true, std::memory_order_release); }
  // True exactly once: the first call after a page has rendered.
  bool takeFirstRender() {
    if (remembered || !pageRendered.load(std::memory_order_acquire)) return false;
    remembered = true;
    return true;
  }

  // In memory only: every path that can act on it (sleep, reader exit) saves
  // APP_STATE itself, and skipping the write keeps Continue Reading free of SD
  // writes when the same book is remembered again a moment later.
  static void forgetOpenBook();
  // Sets APP_STATE.openEpubPath (saved, so a silent restart from a reader
  // sub-activity finds it) and adds the recents entry.
  static void rememberOpenBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& thumbBmpPath);
};

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  std::unique_ptr<Epub> loadEpub(const std::string& path) const;
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isImageFile(const std::string& path);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath)
      : Activity("Reader", renderer, mappedInput), initialBookPath(std::move(initialBookPath)) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
