#pragma once
#include <memory>

#include "../Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;
class Txt;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  std::unique_ptr<Epub> loadEpub(const std::string& path);
  // Reason the last loadEpub() refused a protected book, in Epub's wording.
  // Empty for every other kind of load failure.
  std::string protectionError;
  // Shows the refusal as a dialog and returns true when it took over the
  // screen; false means the caller should fall back to onGoBack().
  bool showProtectionFailure();
  // Wi-Fi -> NTP -> reopen, for a loan whose due date could not be checked
  // because the clock was never set.
  void beginLoanTimeSync();
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isBmpFile(const std::string& path);

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
