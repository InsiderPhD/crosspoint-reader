#pragma once

#include <cstdint>

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3, Percent = 4 };

// Represents a CSS length value with its unit, allowing deferred resolution to pixels
struct CssLength {
  float value = 0.0f;
  CssUnit unit = CssUnit::Pixels;

  CssLength() = default;
  CssLength(const float v, const CssUnit u) : value(v), unit(u) {}

  // Convenience constructor for pixel values (most common case)
  explicit CssLength(const float pixels) : value(pixels) {}

  // Returns true if this length can be resolved to pixels with the given context.
  // Percentage units require a non-zero containerWidth to resolve.
  [[nodiscard]] bool isResolvable(const float containerWidth = 0) const {
    return unit != CssUnit::Percent || containerWidth > 0;
  }

  // Resolve to pixels given the current em size (font line height)
  // containerWidth is needed for percentage units (e.g. viewport width)
  [[nodiscard]] float toPixels(const float emSize, const float containerWidth = 0) const {
    switch (unit) {
      case CssUnit::Em:
      case CssUnit::Rem:
        return value * emSize;
      case CssUnit::Points:
        return value * 1.33f;  // Approximate pt to px conversion
      case CssUnit::Percent:
        return value * containerWidth / 100.0f;
      default:
        return value;
    }
  }

  // Resolve to int16_t pixels (for BlockStyle fields)
  [[nodiscard]] int16_t toPixelsInt16(const float emSize, const float containerWidth = 0) const {
    return static_cast<int16_t>(toPixels(emSize, containerWidth));
  }
};

// Font style options matching CSS font-style property
enum class CssFontStyle : uint8_t { Normal = 0, Italic = 1 };

// Font weight options - CSS supports 100-900, we simplify to normal/bold
enum class CssFontWeight : uint8_t { Normal = 0, Bold = 1 };

// Text decoration options
enum class CssTextDecoration : uint8_t { None = 0, Underline = 1 };

// Display options. None hides the element; Inline matters for images only (see
// ChapterHtmlSlimParser's inline-image rule). Values are stored in the CSS rule cache.
enum class CssDisplay : uint8_t { Block = 0, None = 1, Inline = 2 };

// Text direction (LTR / RTL)
enum class CssTextDirection : uint8_t { Ltr = 0, Rtl = 1 };

// Vertical alignment options for inline elements (e.g. superscript/subscript)
enum class CssVerticalAlign : uint8_t { Baseline = 0, Super = 1, Sub = 2 };

// Border line styles the renderer can draw. Values are stored in the CSS rule cache and in
// section files (PageBorderBox): never renumber.
enum class CssBorderStyle : uint8_t { None = 0, Solid = 1, Double = 2, Dotted = 3, Dashed = 4 };

// One edge of a CSS border, packed into a byte so the four of them fit the padding CssStyle
// already had (see the size assert below): rules are held in RAM for a whole section build.
struct CssBorderSide {
  static constexpr uint8_t MAX_WIDTH = 8;
  uint8_t width : 4;      // pixels, 0..MAX_WIDTH. CSS's initial value is `medium` (3px).
  uint8_t styleBits : 3;  // a CssBorderStyle

  constexpr CssBorderSide() : width(3), styleBits(0) {}
  [[nodiscard]] CssBorderStyle style() const { return static_cast<CssBorderStyle>(styleBits); }
  void setStyle(const CssBorderStyle style) { styleBits = static_cast<uint8_t>(style); }
  [[nodiscard]] bool visible() const { return width != 0 && styleBits != 0; }
  // One-byte wire form, used by both caches.
  [[nodiscard]] uint8_t packed() const { return static_cast<uint8_t>(width | (styleBits << 4)); }
  static bool unpack(const uint8_t byte, CssBorderSide& out) {
    const uint8_t width = byte & 0x0F;
    const uint8_t style = byte >> 4;
    if (width > MAX_WIDTH || style > static_cast<uint8_t>(CssBorderStyle::Dashed)) return false;
    out.width = width;
    out.styleBits = style;
    return true;
  }
};
static_assert(sizeof(CssBorderSide) == 1, "CssBorderSide must stay one byte");

// Bitmask for tracking which properties have been explicitly set
struct CssPropertyFlags {
  uint32_t textAlign : 1;
  uint32_t fontStyle : 1;
  uint32_t fontWeight : 1;
  uint32_t textDecoration : 1;
  uint32_t textIndent : 1;
  uint32_t marginTop : 1;
  uint32_t marginBottom : 1;
  uint32_t marginLeft : 1;
  uint32_t marginRight : 1;
  uint32_t paddingTop : 1;
  uint32_t paddingBottom : 1;
  uint32_t paddingLeft : 1;
  uint32_t paddingRight : 1;
  uint32_t imageHeight : 1;
  uint32_t imageWidth : 1;
  uint32_t display : 1;
  uint32_t direction : 1;
  uint32_t verticalAlign : 1;
  // The four below are the VALUE as well as the "defined" mark: only the setting that
  // changes layout is recorded (page-break always, white-space pre*, a non-white
  // background). A rule that only restates the default (`page-break-after: avoid` on every
  // heading, `background: #fff` on body) therefore still counts as a no-op and is not
  // stored. The price: a later rule cannot switch one of these back off.
  uint32_t pageBreakBefore : 1;
  uint32_t pageBreakAfter : 1;
  uint32_t whiteSpacePre : 1;
  uint32_t shaded : 1;
  // Per-edge masks (bit 0 top, 1 right, 2 bottom, 3 left) for CssStyle::borders: which
  // edges had their line style / their width set.
  uint32_t borderStyle : 4;
  uint32_t borderWidth : 4;

  CssPropertyFlags()
      : textAlign(0),
        fontStyle(0),
        fontWeight(0),
        textDecoration(0),
        textIndent(0),
        marginTop(0),
        marginBottom(0),
        marginLeft(0),
        marginRight(0),
        paddingTop(0),
        paddingBottom(0),
        paddingLeft(0),
        paddingRight(0),
        imageHeight(0),
        imageWidth(0),
        display(0),
        direction(0),
        verticalAlign(0),
        pageBreakBefore(0),
        pageBreakAfter(0),
        whiteSpacePre(0),
        shaded(0),
        borderStyle(0),
        borderWidth(0) {}

  [[nodiscard]] bool anySet() const {
    return textAlign || fontStyle || fontWeight || textDecoration || textIndent || marginTop || marginBottom ||
           marginLeft || marginRight || paddingTop || paddingBottom || paddingLeft || paddingRight || imageHeight ||
           imageWidth || display || direction || verticalAlign || pageBreakBefore || pageBreakAfter || whiteSpacePre ||
           shaded || borderStyle || borderWidth;
  }

  void clearAll() {
    textAlign = fontStyle = fontWeight = textDecoration = textIndent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
    imageHeight = imageWidth = display = direction = verticalAlign = 0;
    pageBreakBefore = pageBreakAfter = whiteSpacePre = shaded = borderStyle = borderWidth = 0;
  }
};

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
// Length values are stored as CssLength (value + unit) for deferred resolution
// Cache serializes defined flags as uint32_t with bit indices 0..29.
static_assert(sizeof(CssPropertyFlags) <= sizeof(uint32_t),
              "CssPropertyFlags exceeds 32 bits; update cache read/write in CssParser.cpp");

struct CssStyle {
  CssTextAlign textAlign = CssTextAlign::Left;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration textDecoration = CssTextDecoration::None;
  CssTextDirection direction = CssTextDirection::Ltr;
  // Here, in what used to be padding ahead of the first CssLength, so the struct did not
  // grow when the border edges were added below.
  CssDisplay display = CssDisplay::Block;                       // display property
  CssVerticalAlign verticalAlign = CssVerticalAlign::Baseline;  // vertical-align (super/sub positioning)

  CssLength textIndent;      // First-line indent (deferred resolution)
  CssLength marginTop;       // Vertical spacing before block
  CssLength marginBottom;    // Vertical spacing after block
  CssLength marginLeft;      // Horizontal spacing left of block
  CssLength marginRight;     // Horizontal spacing right of block
  CssLength paddingTop;      // Padding before
  CssLength paddingBottom;   // Padding after
  CssLength paddingLeft;     // Padding left
  CssLength paddingRight;    // Padding right
  CssLength imageHeight;     // Height for img (e.g. 2em) – width derived from aspect ratio when only height set
  CssLength imageWidth;      // Width for img when both or only width set
  CssBorderSide borders[4];  // top, right, bottom, left; see defined.borderStyle / borderWidth

  CssPropertyFlags defined;  // Tracks which properties were explicitly set

  [[nodiscard]] bool hasVisibleBorder() const {
    return borders[0].visible() || borders[1].visible() || borders[2].visible() || borders[3].visible();
  }

  // Apply properties from another style, only overwriting if the other style
  // has that property explicitly defined
  void applyOver(const CssStyle& base) {
    if (base.hasTextAlign()) {
      textAlign = base.textAlign;
      defined.textAlign = 1;
    }
    if (base.hasFontStyle()) {
      fontStyle = base.fontStyle;
      defined.fontStyle = 1;
    }
    if (base.hasFontWeight()) {
      fontWeight = base.fontWeight;
      defined.fontWeight = 1;
    }
    if (base.hasTextDecoration()) {
      textDecoration = base.textDecoration;
      defined.textDecoration = 1;
    }
    if (base.hasTextIndent()) {
      textIndent = base.textIndent;
      defined.textIndent = 1;
    }
    if (base.hasMarginTop()) {
      marginTop = base.marginTop;
      defined.marginTop = 1;
    }
    if (base.hasMarginBottom()) {
      marginBottom = base.marginBottom;
      defined.marginBottom = 1;
    }
    if (base.hasMarginLeft()) {
      marginLeft = base.marginLeft;
      defined.marginLeft = 1;
    }
    if (base.hasMarginRight()) {
      marginRight = base.marginRight;
      defined.marginRight = 1;
    }
    if (base.hasPaddingTop()) {
      paddingTop = base.paddingTop;
      defined.paddingTop = 1;
    }
    if (base.hasPaddingBottom()) {
      paddingBottom = base.paddingBottom;
      defined.paddingBottom = 1;
    }
    if (base.hasPaddingLeft()) {
      paddingLeft = base.paddingLeft;
      defined.paddingLeft = 1;
    }
    if (base.hasPaddingRight()) {
      paddingRight = base.paddingRight;
      defined.paddingRight = 1;
    }
    if (base.hasImageHeight()) {
      imageHeight = base.imageHeight;
      defined.imageHeight = 1;
    }
    if (base.hasImageWidth()) {
      imageWidth = base.imageWidth;
      defined.imageWidth = 1;
    }
    if (base.hasDisplay()) {
      display = base.display;
      defined.display = 1;
    }
    if (base.hasDirection()) {
      direction = base.direction;
      defined.direction = 1;
    }
    if (base.hasVerticalAlign()) {
      verticalAlign = base.verticalAlign;
      defined.verticalAlign = 1;
    }
    if (base.defined.pageBreakBefore) defined.pageBreakBefore = 1;
    if (base.defined.pageBreakAfter) defined.pageBreakAfter = 1;
    if (base.defined.whiteSpacePre) defined.whiteSpacePre = 1;
    if (base.defined.shaded) defined.shaded = 1;
    for (uint8_t edge = 0; edge < 4; ++edge) {
      const uint8_t bit = static_cast<uint8_t>(1U << edge);
      if (base.defined.borderStyle & bit) borders[edge].styleBits = base.borders[edge].styleBits;
      if (base.defined.borderWidth & bit) borders[edge].width = base.borders[edge].width;
    }
    defined.borderStyle |= base.defined.borderStyle;
    defined.borderWidth |= base.defined.borderWidth;
  }

  [[nodiscard]] bool hasTextAlign() const { return defined.textAlign; }
  [[nodiscard]] bool hasFontStyle() const { return defined.fontStyle; }
  [[nodiscard]] bool hasFontWeight() const { return defined.fontWeight; }
  [[nodiscard]] bool hasTextDecoration() const { return defined.textDecoration; }
  [[nodiscard]] bool hasTextIndent() const { return defined.textIndent; }
  [[nodiscard]] bool hasMarginTop() const { return defined.marginTop; }
  [[nodiscard]] bool hasMarginBottom() const { return defined.marginBottom; }
  [[nodiscard]] bool hasMarginLeft() const { return defined.marginLeft; }
  [[nodiscard]] bool hasMarginRight() const { return defined.marginRight; }
  [[nodiscard]] bool hasPaddingTop() const { return defined.paddingTop; }
  [[nodiscard]] bool hasPaddingBottom() const { return defined.paddingBottom; }
  [[nodiscard]] bool hasPaddingLeft() const { return defined.paddingLeft; }
  [[nodiscard]] bool hasPaddingRight() const { return defined.paddingRight; }
  [[nodiscard]] bool hasImageHeight() const { return defined.imageHeight; }
  [[nodiscard]] bool hasImageWidth() const { return defined.imageWidth; }
  [[nodiscard]] bool hasDisplay() const { return defined.display; }
  [[nodiscard]] bool hasDirection() const { return defined.direction; }
  [[nodiscard]] bool hasVerticalAlign() const { return defined.verticalAlign; }

  void reset() {
    textAlign = CssTextAlign::Left;
    fontStyle = CssFontStyle::Normal;
    fontWeight = CssFontWeight::Normal;
    textDecoration = CssTextDecoration::None;
    direction = CssTextDirection::Ltr;
    textIndent = CssLength{};
    marginTop = marginBottom = marginLeft = marginRight = CssLength{};
    paddingTop = paddingBottom = paddingLeft = paddingRight = CssLength{};
    imageHeight = imageWidth = CssLength{};
    display = CssDisplay::Block;
    verticalAlign = CssVerticalAlign::Baseline;
    for (auto& side : borders) side = CssBorderSide{};
    defined.clearAll();
  }
};

// A rule is held in RAM (one per selector) for the length of a section build, which is the
// tightest heap state the reader has. New properties must be packed into what is here.
static_assert(sizeof(CssStyle) <= 104, "CssStyle grew: every stored CSS rule pays for it during section builds");
