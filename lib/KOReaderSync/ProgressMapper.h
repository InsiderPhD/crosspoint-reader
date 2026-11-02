#pragma once
#include <Epub.h>

#include <memory>
#include <optional>
#include <string>

#include "CrossPointPosition.h"
#include "KOReaderSyncClient.h"

class GfxRenderer;

/**
 * KOReader position representation.
 */
struct KOReaderPosition {
  std::string xpath;  // XPath-like progress string
  float percentage;   // Progress percentage (0.0 to 1.0)
};

/**
 * Maps between CrossPoint and KOReader position formats.
 *
 * CrossPoint tracks position as (spineIndex, pageNumber).
 * KOReader uses XPath-like strings + percentage.
 *
 * Since CrossPoint discards HTML structure during parsing, we generate
 * synthetic XPath strings based on spine index, using percentage as the
 * primary sync mechanism.
 */
class ProgressMapper {
 public:
  /**
   * Convert CrossPoint position to KOReader format.
   *
   * @param epub The EPUB book
   * @param pos CrossPoint position
   * @return KOReader position
   */
  static KOReaderPosition toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos);

  /**
   * Convert KOReader position to CrossPoint format.
   *
   * Note: The returned pageNumber may be approximate since different
   * rendering settings produce different page counts.
   *
   * @param epub The EPUB book
   * @param koPos KOReader position
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                         int currentSpineIndex = -1, int totalPagesInCurrentSpine = 0);

  /**
   * Replace a toCrossPoint() page estimate with one from the target chapter's
   * cached layout: the visible-text offset LUT first, then the list-item,
   * anchor and paragraph LUTs. Sets hasMappedPage when one of them answers;
   * leaves the estimate alone when the chapter has no cached layout.
   */
  static void refinePageFromLayout(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, CrossPointPosition& pos);

  /**
   * Rich crosspoint-sync position for an upload: percentage quantized, the
   * layout hints, and the same XPath as the standard progress field.
   */
  static KOReaderRichPosition toRichPosition(const CrossPointPosition& pos, const KOReaderPosition& koPos);

  /**
   * Map a rich position downloaded from a crosspoint-sync server when its XPath
   * could not be resolved to content: same chapter page count means the page
   * transfers as is, otherwise the paragraph LUT, otherwise the page fraction.
   * std::nullopt when the spine is out of range or has no cached layout.
   */
  static std::optional<CrossPointPosition> fromRichPosition(const std::shared_ptr<Epub>& epub,
                                                            const KOReaderRichPosition& rich);

 private:
  /**
   * Generate a fallback XPath by streaming the spine item's XHTML and resolving
   * a paragraph/text position from intra-spine progress.
   * Produces a full ancestry path such as
   * /body/DocFragment[3]/body/p[42]/text().17.
   */
  static std::string generateXPath(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);
};
