#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <WiFi.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "SilentRestart.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "util/WifiTimeSync.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  protectionError.clear();
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  bool borrowedFramebuffer = false;
  bool loaded = false;
  {
    // A stale book.bin (cache-version bump) forces a metadata rebuild here, and the
    // rebuild's inflate needs one 32KB CONTIGUOUS dictionary — the allocation that
    // fails first once WiFi/BLE have ever fragmented the heap (largest block caps
    // just under 32768 for the rest of the boot). Lend the idle framebuffer as the
    // dictionary, exactly as Section.cpp does for section builds: e-ink is bistable
    // so the panel keeps its image, and nothing renders while load() runs.
    InflateScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
    borrowedFramebuffer = scratch.active();
    loaded = epub->load(true, SETTINGS.embeddedStyle == 0);
  }  // lease released: the framebuffer is ours to paint into again

  // The lease left dictionary bytes in the framebuffer. Clear it — buffer only,
  // so the panel keeps showing the loading popup — before anything composites a
  // popup over it instead of repainting in full. Section::createSectionFile does
  // the same after its own lease.
  if (borrowedFramebuffer) {
    renderer.clearScreen();
  }

  if (loaded) {
    return epub;
  }

  // The book dies with this scope, so carry its refusal reason out.
  protectionError = epub->getProtectionError();
  LOG_ERR("READER", "Failed to load epub%s%s", protectionError.empty() ? "" : ": ", protectionError.c_str());
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint"));
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = std::unique_ptr<Txt>(new Txt(path, "/.crosspoint"));
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc)));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt)));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);  // BmpViewerActivity draws its own loading popup
    return;
  }

  // Opening a book is synchronous and can take several seconds on a cold cache:
  // the metadata parse (and, on a cache-version bump, a full inflate of the
  // container) here, then the per-spine page-count scan in the reader's own
  // onEnter(). Until the reader's first page lands, nothing repaints, so the
  // device looks frozen with the browser still on screen. Paint the popup over
  // whatever the previous activity left in the framebuffer and push it before
  // any of that starts. E-ink is bistable, so it stays visible for the whole
  // load -- including while loadEpub() lends the framebuffer to the inflate --
  // until the reader replaces it with the page or with a build's "Indexing..."
  // popup.
  GUI.drawPopup(renderer, tr(STR_LOADING));

  if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      // A protected book that could not be opened explains itself rather than
      // bouncing silently back to the browser.
      if (!showProtectionFailure()) onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

// A protected book the device could not open is a user-facing state, not a
// silent failure: say why, and where the reason is a clock the device never
// had a chance to set, offer to go and set it. The exact strings compared here
// are the ones openProtectedBook() produces (lib/Epub/ContentProtection.cpp).
bool ReaderActivity::showProtectionFailure() {
  if (protectionError.empty()) return false;

  StrId message = StrId::STR_DRM_PROTECTED_FILE;
  bool offerSync = false;
  std::string detail;
  if (protectionError == "access expired") {
    message = StrId::STR_LOAN_EXPIRED;
  } else if (protectionError == "loan date unverified") {
    message = StrId::STR_LOAN_TIME_UNVERIFIED;
    offerSync = true;
  } else if (protectionError == "no content access key on this device") {
    // The book is fine; this device has never been authorised (or the
    // credential the plugin writes to /.crosspoint/content.key is gone). Say
    // that, because it is the one case the reader's user can actually fix.
    message = StrId::STR_CONTENT_NOT_AUTHORISED;
  } else if (protectionError.rfind("cannot open protected content: ", 0) == 0) {
    // Rights/key failure. The tail is the crypto layer's own wording — not
    // translatable, but it is the difference between "wrong account", "no
    // rights sidecar" and "corrupt file", so show it rather than making the
    // reader go and read a serial log.
    message = StrId::STR_CONTENT_OPEN_FAILED;
    detail = protectionError.substr(strlen("cannot open protected content: "));
  }

  std::string body = I18N.get(message);
  if (!detail.empty()) {
    body += "\n";
    body += detail;
  }

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_ERROR_MSG), body),
                         [this, offerSync](const ActivityResult& result) {
                           if (offerSync && !result.isCancelled) {
                             beginLoanTimeSync();
                             return;
                           }
                           onGoBack();
                         });
  return true;
}

void ReaderActivity::beginLoanTimeSync() {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             onGoBack();
                             return;
                           }
                           GUI.drawPopup(renderer, tr(STR_SYNCING_TIME));
                           WifiTimeSync::attemptIfStale();
                           WiFi.disconnect(false);
                           delay(30);
                           // Reboot back into this book with a clean heap; the
                           // Wi-Fi session has fragmented it, and opening a
                           // protected book needs contiguous blocks.
                           APP_STATE.openEpubPath = currentBookPath;
                           APP_STATE.saveToFile();
                           silentRestartToReader();
                         });
}

void ReaderActivity::onGoBack() { finish(); }
