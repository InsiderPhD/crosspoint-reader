# File Formats

## `book.bin`

> **Current version: 12** (`BOOK_CACHE_VERSION` in `lib/Epub/Epub/BookMetadataCache.cpp`).
> v12 did not change the layout: multiple `<dc:creator>` authors are now joined with
> `" & "` instead of `", "`, so the Authors folder view can split co-authors without
> breaking "Last, First" names. The bump exists to force stale `book.bin` files to rebuild.
>
> The ImHex pattern below documents the original v3 layout
> and has not been kept fully in sync. Since then the metadata section gained, in order:
> `language` (v5); `tags`, `seriesName`, `seriesIndex` (v10); and `publisher`, `pubDate`,
> `rating`, `bookshelf` (v11) — each a length-prefixed UTF-8 string appended after
> `textReferenceHref` (12 metadata strings total). `rating` is the raw Calibre 0–10 value;
> `bookshelf` is the `#value#` of the `calibre:user_metadata:#bookshelf` custom column. The
> book **description** is NOT in `book.bin`; it is stored separately in a `desc.bin` sidecar
> in the book's cache directory (HTML-stripped plain UTF-8, capped at
> `Epub::MAX_DESCRIPTION_BYTES`) and read lazily by the Book Details view so it never loads
> on the Library hot path.

### Version 3

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 3
#define MAX_STRING_LENGTH 65535

// === String Structure ===

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

// === Metadata Structure ===

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
} [[comment("Book metadata information")]];

// === Spine Entry Structure ===

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative size in bytes"), color("FF6B6B")]];
    s16 tocIndex [[comment("Index into TOC (-1 if none)"), color("4ECDC4")]];
} [[comment("Spine entry defining reading order")]];

// === TOC Entry Structure ===

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level (0-255)"), color("95E1D3")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)"), color("F38181")]];
} [[comment("Table of contents entry")]];

// === Book Bin Structure ===

struct BookBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];

    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables"), color("6BCB77")]];
    u16 spineCount [[comment("Number of spine entries"), color("4D96FF")]];
    u16 tocCount [[comment("Number of TOC entries"), color("FF6B9D")]];

    // Metadata section
    Metadata metadata [[comment("Book metadata")]];

    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    // Lookup Tables
    u32 spineLut[spineCount] [[comment("Spine entry offsets"), color("4D96FF")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets"), color("FF6B9D")]];

    // Data Entries
    SpineEntry spines[spineCount] [[comment("Spine entries (reading order)")]];
    TocEntry toc[tocCount] [[comment("Table of contents entries")]];
};

// === File Parsing ===

BookBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

> **Current version: 40** (`SECTION_FILE_VERSION` in `lib/Epub/Epub/Section.cpp`).
> The ImHex pattern below documents v24 and has not been kept in sync. Changes since:
> v30 inline ("on page") footnotes; v31–v33 KOReader xpath/paragraph sync data and
> nested block styles; v34 `<pre>` blocks carry a monospace font override; v35 hanging
> indent stored as a signed `xpos`; v36 bionic-reading spans; v37 bionic stored as whole
> words and expanded at layout time; v38 right-sized inline footnote storage; v39 image
> percentage widths resolve against the wrapper element; v40 images stay in the layout
> when the decoder cannot open them (previously dropped, and the drop was cached).
>
> Any change to the on-disk structure **must** bump this constant — a stale cache read
> against a new layout is how you get a hard fault, not a graceful failure.

### Version 24

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 24
#define MAX_STRING_LENGTH 65535

// === String Structure ===

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

// === Page Structure ===

enum PageElementTag : u8 {
    PageLine = 1,
    PageImage = 2,
    PageHorizontalRule = 3
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3
};

enum BlockStyle : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
};

struct PageLine {
  s16 xPos;
  s16 yPos;
  u16 wordCount;
  String words[wordCount];
  u16 wordXPos[wordCount];
  WordStyle wordStyle[wordCount];
  BlockStyle blockStyle;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    String imagePath;
    s16 width;
    s16 height;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct PageElement {
    u8 pageElementType;
    if (pageElementType == 1) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == 2) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == 3) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];
};

// === Section Bin Structure ===

struct SectionBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];

    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    // Cache busting parameters
    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    u16 viewportWidth;
    u16 vieportHeight;
    u16 pageCount;
    u32 lutOffset;

    Page page[pageCount];

    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    // Lookup Tables
    u32 lut[pageCount];
};

// === File Parsing ===

SectionBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```
