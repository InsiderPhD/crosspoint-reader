#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <InflateReader.h>

#include "BookSettings.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "RecentBooksStore.h"
#include "SdCardFontGlobals.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"

void RenderedBookGate::forgetOpenBook() { APP_STATE.openEpubPath.clear(); }

void RenderedBookGate::rememberOpenBook(const std::string& path, const std::string& title, const std::string& author,
                                        const std::string& thumbBmpPath) {
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();  // no card write when the path is the one already on disk
  RECENT_BOOKS.addBook(path, title, author, thumbBmpPath);
}

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isImageFile(const std::string& path) { return FsHelpers::hasImageExtension(path); }

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) const {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  const unsigned long loadStartMs = millis();
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

  // Resuming a book on boot runs this between the boot screen and the first page,
  // and on a cache-version bump it is a full inflate + metadata rebuild. Time it:
  // "the device sat there for a minute" is otherwise indistinguishable from a
  // slow panel or a slow SD mount. Pairs with the BOOT_PHASE lines in main.cpp.
  LOG_INF("READER", "Epub metadata load: %lu ms", millis() - loadStartMs);

  if (loaded) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
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
  if (isImageFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);  // BmpViewerActivity (BMP/JPEG/PNG) draws its own loading popup
    return;
  }

  // Forget the remembered book until this one has rendered a page (upstream
  // a70ade72e): if it fails to load or index, sleeping on the failure must wake
  // to Home, not back into the same unreadable book. The reader that renders
  // it writes the path back (RenderedBookGate).
  RenderedBookGate::forgetOpenBook();

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
  //
  // An EPUB whose book.bin (and CSS rules cache, when styles are on) is already
  // current skips it: load() is then a header read, and the popup would only add
  // a panel refresh in front of the reader's first page. XTC/TXT and any EPUB
  // that has to build or rebuild a cache still get the popup.
  const bool epubCacheReady = !isXtcFile(initialBookPath) && !isTxtFile(initialBookPath) &&
                              Epub::hasCache(initialBookPath, "/.crosspoint", SETTINGS.embeddedStyle == 0);
  if (!epubCacheReady) {
    GUI.drawPopup(renderer, tr(STR_LOADING));
  }

  // Per-book reader settings go live here, before the book is loaded: a book
  // that overrides the font swaps the SD font now, while the heap is still
  // empty of the book, instead of in the middle of the reading session.
  // XTC/TXT have no per-book settings; end() clears anything left behind.
  BookSettings::end();
  if (!isXtcFile(initialBookPath) && !isTxtFile(initialBookPath)) {
    BookSettings::begin(Epub::cachePathFor(initialBookPath, "/.crosspoint"));
    if (BookSettings::hasOverrides()) ensureSdFontLoaded();
  }

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
      BookSettings::end();
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }
