#include "BookDetailsActivity.h"

#include <Bitmap.h>
#include <BookFusionMetaStore.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int COVER_TO_TEXT_GAP = 16;
constexpr int ROW_V_PAD = 6;

// Draw a single "Label: value" table row; returns the y for the next row.
int drawInfoRow(GfxRenderer& renderer, int x, int y, int width, const char* label, const std::string& value) {
  // Fixed gap between label and value: trailing spaces have zero advance width in
  // the font, so we can't rely on a "Label: " space to separate them.
  constexpr int LABEL_VALUE_GAP = 6;
  const std::string lbl = std::string(label) + ":";
  const int lblWidth = renderer.getTextWidth(UI_10_FONT_ID, lbl.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, y, lbl.c_str(), true, EpdFontFamily::BOLD);
  const int valueX = x + lblWidth + LABEL_VALUE_GAP;
  const std::string val = renderer.truncatedText(UI_10_FONT_ID, value.c_str(), std::max(0, width - lblWidth - LABEL_VALUE_GAP));
  renderer.drawText(UI_10_FONT_ID, valueX, y, val.c_str());
  return y + renderer.getLineHeight(UI_10_FONT_ID) + ROW_V_PAD;
}

void drawCover(GfxRenderer& renderer, const Rect& rect, const std::string& coverPath) {
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  const auto drawFallback = [&]() {
    const char* label = tr(STR_BOOK);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - textWidth) / 2, rect.y + rect.height / 2, label, true,
                      EpdFontFamily::BOLD);
  };

  if (coverPath.empty()) {
    drawFallback();
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("BDT", coverPath, file)) {
    drawFallback();
    return;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() == BmpReaderError::Ok) {
    renderer.drawBitmap(bitmap, rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4);
  } else {
    drawFallback();
  }
  file.close();
}
}  // namespace

void BookDetailsActivity::freeDescBuffer() {
  if (descBuffer) {
    free(descBuffer);
    descBuffer = nullptr;
  }
  descLength = 0;
}

void BookDetailsActivity::loadMetadata() {
  // Series/tags/description live only in EPUB OPF metadata (Calibre). Other formats
  // still get a basic details page (title/author/progress/placeholder).
  if (!FsHelpers::hasEpubExtension(bookPath)) {
    return;
  }

  Epub epub(bookPath, "/.crosspoint");
  if (!epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true)) {
    LOG_ERR("BDT", "Could not load EPUB metadata for details: %s", bookPath.c_str());
    return;
  }

  if (title.empty()) title = epub.getTitle();
  if (author.empty()) author = epub.getAuthor();
  seriesName = epub.getSeriesName();
  seriesNumber = epub.getSeriesIndex();
  bookshelf = epub.getBookshelf();
  published = epub.getPublishedDate();
  publisher = epub.getPublisher();
  language = epub.getLanguage();
  tags = epub.getTags();
  rating = epub.getRating();

  // BookFusion organisational metadata (categories / bookshelves / lists) isn't in
  // the EPUB; it's persisted as a sidecar at download time. The Calibre #bookshelf
  // column (if any) takes precedence for the Bookshelf row; otherwise use the API's.
  BookFusionMeta bfMeta;
  if (BookFusionMetaStore::load(epub.getCachePath(), bfMeta)) {
    categories = bfMeta.categories;
    lists = bfMeta.lists;
    if (bookshelf.empty()) bookshelf = bfMeta.bookshelves;
  }

  // Cover at the Lyra Library grid size.
  const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  const std::string thumbPath = epub.getThumbBmpPath(coverHeight);
  if (!Storage.exists(thumbPath.c_str())) {
    epub.generateThumbBmp(coverHeight);
  }
  if (Storage.exists(thumbPath.c_str())) {
    coverPath = thumbPath;
  }

  // Read the description sidecar into one bounded buffer.
  if (epub.hasDescription()) {
    FsFile descFile;
    if (Storage.openFileForRead("BDT", epub.getDescriptionPath(), descFile)) {
      const size_t fileSize = descFile.size();
      const size_t toRead = std::min<size_t>(fileSize, Epub::MAX_DESCRIPTION_BYTES);
      descBuffer = static_cast<char*>(malloc(toRead + 1));
      if (descBuffer) {
        const size_t readBytes = descFile.read(reinterpret_cast<uint8_t*>(descBuffer), toRead);
        descBuffer[readBytes] = '\0';
        descLength = readBytes;
      } else {
        LOG_ERR("BDT", "malloc failed for description buffer: %d bytes", static_cast<int>(toRead + 1));
      }
      descFile.close();
    }
  }
}

void BookDetailsActivity::onEnter() {
  Activity::onEnter();
  scrollOffset = 0;
  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  waitForBackRelease = false;

  // loadMetadata() can block (building a missing cache, generating the cover thumbnail).
  // Paint a "Loading…" screen synchronously first so the previous screen doesn't freeze.
  loading = true;
  requestUpdateAndWait();
  loadMetadata();
  loading = false;
  requestUpdate();
}

void BookDetailsActivity::onExit() {
  Activity::onExit();
  freeDescBuffer();
}

void BookDetailsActivity::loop() {
  if (waitForBackRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      waitForBackRelease = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (waitForConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      waitForConfirmRelease = false;
    }
    return;
  }

  // Page-at-a-time scroll through the description (clamped against maxScrollOffset in render()).
  const int pageStep = std::max(1, descVisibleLines - 1);
  buttonNavigator.onNext([this, pageStep] {
    scrollOffset += pageStep;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, pageStep] {
    scrollOffset = std::max(0, scrollOffset - pageStep);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && Storage.exists(bookPath.c_str())) {
    onSelectBook(bookPath);
  }
}

void BookDetailsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOK_INFO), nullptr);

  if (loading) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    const auto loadingLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, loadingLabels.btn1, loadingLabels.btn2, loadingLabels.btn3, loadingLabels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int viewportBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  // Cover (left), sized to the Lyra Library grid but capped so it can't crowd out the
  // description on short (landscape) viewports.
  int coverHeight = metrics.homeCoverHeight;
  coverHeight = std::min(coverHeight, std::max(80, viewportBottom - contentTop - 80));
  const int coverWidth = coverHeight * 3 / 5;  // matches the 0.6 thumbnail aspect
  const Rect coverRect{metrics.contentSidePadding, contentTop, coverWidth, coverHeight};
  drawCover(renderer, coverRect, coverPath);

  // Metadata table (right column).
  const int textX = coverRect.x + coverRect.width + COVER_TO_TEXT_GAP;
  const int textWidth = pageWidth - textX - metrics.contentSidePadding;

  int y = contentTop + 4;
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, title.c_str(), textWidth, 2, EpdFontFamily::BOLD);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }
  y += ROW_V_PAD;

  // Each row is drawn only when its value is present, so missing metadata leaves no gap.
  if (!author.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_AUTHOR), author);
  if (!seriesName.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_SERIES), seriesName);
  if (!seriesNumber.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_NUMBER), seriesNumber);
  if (!bookshelf.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_BOOKSHELF), bookshelf);
  if (!categories.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_CATEGORIES), categories);
  if (!lists.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_LISTS), lists);
  if (!published.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_PUBLISHED), published);
  if (!publisher.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_PUBLISHER), publisher);
  if (!language.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_LANGUAGE), language);
  if (!tags.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_TAGS), tags);
  if (!rating.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_RATING), rating);
  {
    const std::string progress =
        progressPercent < 0 ? std::string(tr(STR_BOOK_INFO_NOT_STARTED)) : std::to_string(progressPercent) + "%";
    y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_PROGRESS), progress);
  }

  // Description (full width, below cover + table).
  int descTop = std::max(coverRect.y + coverRect.height, y) + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, descTop, tr(STR_DESCRIPTION), true, EpdFontFamily::BOLD);
  descTop += lineH + 2;
  renderer.drawLine(metrics.contentSidePadding, descTop, pageWidth - metrics.contentSidePadding, descTop);
  const int descBodyTop = descTop + ROW_V_PAD;

  const int descWidth = pageWidth - metrics.contentSidePadding * 2;
  descVisibleLines = std::max(1, (viewportBottom - descBodyTop) / lineH);

  if (descBuffer && descLength > 0) {
    // Transient wrap (freed at end of render); only the visible window is drawn.
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, descBuffer, descWidth, INT16_MAX);
    const int totalLines = static_cast<int>(lines.size());
    const int maxScrollOffset = std::max(0, totalLines - descVisibleLines);
    scrollOffset = std::min(scrollOffset, maxScrollOffset);

    int ly = descBodyTop;
    for (int i = scrollOffset; i < totalLines && i < scrollOffset + descVisibleLines; i++) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, ly, lines[i].c_str());
      ly += lineH;
    }

    // "more" indicator when there is content below the fold.
    if (scrollOffset < maxScrollOffset) {
      const char* more = "\xE2\x96\xBC";  // ▼
      const int moreWidth = renderer.getTextWidth(UI_10_FONT_ID, more);
      renderer.drawText(UI_10_FONT_ID, pageWidth - metrics.contentSidePadding - moreWidth, viewportBottom - lineH,
                        more);
    }
  } else {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, descBodyTop, tr(STR_NO_DESCRIPTION));
  }

  const char* confirmLabel = Storage.exists(bookPath.c_str()) ? tr(STR_OPEN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
