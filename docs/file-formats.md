# File Formats

## `book.bin`

> **Current version: 13** (`BOOK_CACHE_VERSION` in `lib/Epub/Epub/BookMetadataCache.cpp`).
> v13 did not change the layout either: the metadata build now also writes the publisher page
> list to a `pagelist.bin` sidecar (see below), and the bump makes existing books rebuild so
> they gain it.
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
>
> The first `<dc:identifier>` that is a valid ISBN is likewise kept out of `book.bin`: it is
> written, normalised to ISBN-13 (13 ASCII digits, no newline), to an `isbn.txt` sidecar in the
> same cache directory on the metadata-build pass. Absent when the book has no valid ISBN or its
> cache predates the sidecar. Read only by the Hardcover sync when linking a book.

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

> **Current version: 48** (`SECTION_FILE_VERSION` in `lib/Epub/Epub/Section.cpp`).
> The ImHex pattern below documents v24 and has not been kept in sync. Changes since:
> v30 inline ("on page") footnotes; v31–v33 KOReader xpath/paragraph sync data and
> nested block styles; v34 `<pre>` blocks carry a monospace font override; v35 hanging
> indent stored as a signed `xpos`; v36 bionic-reading spans; v37 bionic stored as whole
> words and expanded at layout time; v38 right-sized inline footnote storage; v39 image
> percentage widths resolve against the wrapper element; v40 images stay in the layout
> when the decoder cannot open them (previously dropped, and the drop was cached);
> v41 a laid-out line's words stored in one flat arena; v42 percentage vertical
> margins/padding resolve against the column's short axis instead of its width, so
> paragraph spacing no longer inflates in landscape; v43 adjacent blocks collapse
> their margins (max) instead of summing them, per CSS; v44 tables are laid out as
> grids in a new page element, `PageTableFragment` (tag 4): `xPos i16, yPos i16,
> columnCount u8, lineStep u16, totalWidth u16, totalHeight u16, hasBorder bool,
> rowCount u16`, then per row `height u16, isHeaderRow bool, cellCount u8`, then per
> cell `isHeader bool, colSpan u8, lineCount u8` followed by `lineCount` TextBlocks.
> Extracted images are now shared per book as `img_<fnv64 of archive path>.<ext>`.
> v45 changes layout only (no structural change): a paragraph long enough to be
> flushed in several passes keeps its first-line indent and top spacing to its
> opening line instead of repeating them at each flush.
> v46 user text-layout controls: the header gains `characterSpacing i8` (half-pixel
> steps), `wordSpacingPercent u8` and `paragraphIndent u8` directly after
> `bionicReadingEnabled` and before `pageCount`, all three compared on load like the
> other render settings; and every TextBlock's style tail gains `characterSpacing i8`
> after `fontOverride`, so a line is drawn with the letter spacing its word positions
> were computed for.
> v47 no structural change: in a book that has a `pagelist.bin`, the anchor map additionally
> carries the ids of page-break markers (`epub:type="pagebreak"`, `role="doc-pagebreak"`, or an
> id of the form `page12` / `pg_12`), recorded against the page being filled when the marker
> is met. Go to Page resolves its targets through that map.
> v48 inline images and publisher decorations. A small image that shares its
> paragraph with text is laid out in the line instead of as a centred block (layout only). It is still
> stored as an ordinary `PageImage` (tag 2), positioned so its bottom edge sits on the
> line's baseline; a line made taller by an image has its `PageLine` moved down by the
> same amount. Publisher decorations add page element tag 5, `PageBorderBox` (a CSS
> border and/or background shade around a block's slice of the page): `xPos i16, yPos
> i16, width u16, height u16`, four edge bytes in top/right/bottom/left order (low nibble
> width in pixels 0-8, high nibble style 0 none / 1 solid / 2 double / 3 dotted /
> 4 dashed; an edge cut by a page break has width 0), then `shaded bool`. Forced CSS page
> breaks and `white-space: pre*` blocks change layout only.
> The CSS rule cache (`css_rules.cache`) moves from version 6 to 8 with it. v7:
> `display: inline` is stored as display value 2 instead of being folded into block (0).
> v8: each rule's fixed payload grows from 65 to 70 bytes: `direction u8` and four border
> edge bytes (same packing as above) follow `verticalAlign`, and the defined-flags word
> gains bits 17 direction, 18 page-break-before, 19 page-break-after, 20 white-space pre,
> 21 shaded background, 22-25 border style set per edge, 26-29 border width set per edge.
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

## Hardcover sidecars

- `/.crosspoint/hardcover.json` — the obfuscated API token (`token_obf`) plus the cached
  account `user_id` and `privacy_setting_id`.
- `/.crosspoint/hardcover_<md5 of epub path>.json` — per-book link: `book_id`, `edition_id`,
  `pages`, `user_book_id`, `read_id`, `started_at`, `synced_pages`, `synced_percent`,
  `synced_day` (last local day ordinal whose progress was replayed as a dated update),
  `finished`, `miss_at` (epoch of the last failed match; retried after a week). Deleting it
  only costs a re-link on the next push, and a replay of the days still in the session log.
- `/.crosspoint/reading_stats.json` `sessionLog` entries carry an optional `endProgress`
  (0-100, book progress when the session ended). Absent on sessions logged before it was
  recorded; no format version bump, older firmware ignores the key.

## Library index: `/.crosspoint/library.clx`

The `CLX1` index behind the Library screen and the Tags / Authors / Series browser when
`libraryUseIndex` is on (Dev tab, default on). A hand port of CrossInk's `lib/LibraryIndex`;
the layout is byte-identical to CrossInk's `library.idx` **format version 7, fold version 3**
(`lib/LibraryIndex/LibraryFormat.h`). The file has a different name here because three
fields mean something different in this fork (below), so the two firmwares must not reuse
each other's indexes.

One sorted snapshot of up to 32,767 `.epub` / `.xtc` books. Every section starts on a
512-byte boundary:

| Section | Contents |
|---|---|
| header | 64-byte `ClixHeader`, padded to 512 |
| folders | one record per folder that holds a book: `u8 pathLen` + path bytes (no trailing `/`). A folder's id is its ordinal |
| records | `bookCount` × exactly 128-byte `ClixRecord`, **in folded-title order** — record `k` is at `recordStart + 128*k`, no offset table |
| permutations | five `u16[bookCount]` arrays of record ordinals: surname order, first-name order, arrival (date added, oldest first), series, tags. Then `u32[bookCount]` packed FAT timestamps in title order |
| names | per record: `u64` FNV-1a hash of the full path, the filename, then five length-prefixed strings (display author, title, source author, series, tags) and a `u32` series position |

```c++
struct ClixHeader {            // 64 bytes
    char magic[4];              // "CLX1"
    u8 formatVersion;           // 7
    u8 foldVersion;             // 3: bump when the fold or a sort key changes
    u8 flags;                   // bit0 ranks degraded, bit1 dedup degraded, bit2 arrival degraded
    u8 metadataEnabled;         // always 1 in this fork
    u16 bookCount;
    u16 folderCount;
    u16 nextFirstSeen;          // monotonic arrival counter, survives rebuilds
    u16 padding1;
    u32 folderStart, folderLen, recordStart, permStart, nameStart, nameLen;
    u32 selfSize;               // expected file size: a truncated or interrupted file cannot validate
    u8 reserved[20];
};

struct ClixRecord {             // exactly 128 bytes
    u32 nameOff;                // into the names section
    u32 fileSize;
    u16 firstSeen;
    u16 folderId;
    u8 nameLen, foldLen, authorKeyLen;
    u8 metadataStatus;          // 0 not attempted, 1 extracted, 2 failed (retried next scan)
    char fold[96];              // folded title: the sort and search key
    char authorKey[12];         // order-insensitive author identity (merges spellings of one person)
    u32 modificationTime;       // packed FAT date/time
};
```

Fork differences from CrossInk (same bytes, different meaning):

- **tags** — the "genre" string holds every `dc:subject`, joined with `", "` exactly as
  `Epub::getTags()` returns them and cut after the last whole tag that fits 255 bytes.
  CrossInk stores only the first subject. The Tags browser groups on all of them.
- **date added** — the "creation time" array carries the file's FAT *modification* time,
  which is what this fork's Date Added sort has always used (`HalFile` exposes no creation time).
- **XTC** — title and author are read from the XTC header; CrossInk indexes XTC by filename only.
- only `.epub` and `.xtc` are indexed; the walk goes 8 folder levels deep and skips
  dot-names and `System Volume Information`.

The index is disposable. A bad magic, an unknown version, a size mismatch or an inconsistent
section layout makes the reader refuse it and the Library fall back to the in-RAM path list
(`library_index.bin`). Deleting `library.clx` only costs a rescan. Installing is
write-then-rename (`library.clx.new`, with `library.clx.bak` as the rollback), so a build
that is cancelled or loses power leaves the previous index in place. While a build runs,
`library.clx.stage`, `.stage.f`, `.prior`, `.rename`, `.order`, `.authors`, `.canon` and
`.runs` exist in `/.crosspoint`; every build removes them when it ends.

### `/.crosspoint/library.clx.dirs` (version 1)

Which directories the index was built from, and the FAT mtime each had. The Library
validates this (one open per directory) instead of walking the card; any mismatch, or a
missing file, means "rescan". `LibraryScan::invalidateIndex()` deletes it (and
`library_index.bin`) — `library.clx` itself is kept so the rescan reuses every unchanged
book's metadata. Written as `library.clx.dirs.new` during a build and renamed into place
only when the build completed.

```c++
struct ClxMarkerHeader {        // 12 bytes, packed
    u32 magic;                  // "CLXD"
    u8 version;                 // 1
    u8 reserved;
    u16 dirCount;
    u16 bookCount;              // must equal library.clx's bookCount
    u16 reserved2;
};
struct ClxMarkerDir {           // dirCount of these, each followed by pathLen path bytes
    u32 modified;               // packed FAT date/time of the directory
    u16 pathLen;                // 1..512
};
```

### `/.crosspoint/library.clm` and `/.crosspoint/library.cld` (version 2)

Metadata parsed from each book, stored the moment it is parsed, so a scan that is cancelled
or fails resumes without re-reading what it already read. Same layout as CrossInk's
`library.meta` / `library.metd`.

`library.clm`: a 32-byte header padded to 512 bytes, then 65,536 16-byte open-addressing
slots (1 MiB, written once on the first scan that parses a book). `library.cld`: append-only
payload records. A book is identified by its path hash, file size and modification time;
any change is a miss. Every slot and payload carries a checksum, so a torn write costs a
re-parse, never wrong metadata. Both files are discarded and recreated when the header does
not validate, 75% of the slots are used, or the payload passes 64 MiB.

```c++
struct CacheHeader {            // 32 bytes at offset 0 of library.clm
    char magic[4];              // "CLM1"
    u8 version;                 // 2
    u8 padding[3];
    u32 slotCount;              // 65536
    u32 usedSlots;
    u8 reserved[16];
};
struct CacheSlot {              // slot i at 512 + 16*i
    u64 pathHash;
    u32 payloadOffset;          // into library.cld
    u32 check;                  // FNV-1a of the two fields above, low bit set
};
struct PayloadHeader {          // 32 bytes, then title, author, series, tags back to back
    u32 magic;                  // "CLMP"
    u64 pathHash;
    u32 fileSize;
    u32 modificationTime;
    u32 seriesPosition;         // sortable encoding of the float series index; 0xFFFFFFFF = none
    u8 titleLen, authorLen, seriesLen, genreLen;
    u32 checksum;
};
```

## Per-book reader settings: `book_settings.bin`

Optional, in the book's cache directory (`/.crosspoint/epub_<hash>/`), so it follows a rename and is
removed with the cache. Written by the reader menu's **Book Settings** editor; a missing file means the
book follows the global settings. Owned by `src/BookSettings.cpp`.

### Version 1

```c++
struct BookSettingsBin {
    u8  version;              // 1
    u8  fieldCount;           // value bytes that follow the mask (14 when written by this firmware)
    u32 overrideMask;         // little-endian; bit i set = values[i] overrides the global setting
    u8  values[fieldCount];   // BookSettings::kFields order, see below
    char sdFontFamilyName[32]; // NUL padded; goes with bit 0, empty = built-in family
};
```

Field order (the index is both the mask bit and the byte; append only): 0 fontFamily, 1 fontSize,
2 lineSpacing, 3 wordSpacing, 4 characterSpacing, 5 paragraphAlignment, 6 screenMargin,
7 extraParagraphSpacing, 8 paragraphIndent, 9 bionicReading, 10 textAntiAliasing,
11 hyphenationEnabled, 12 imageRendering, 13 imageGrayscale.

A reader that knows fewer fields ignores the extra bytes and bits; one that knows more treats the
missing fields as not overridden, so adding a field needs no version bump. An out-of-range value drops
that one override. Values of fields whose mask bit is clear are written but never read.

## Publisher page list: `pagelist.bin`

Optional, in the book's cache directory. Written while `book.bin` is built, from the EPUB 3
`<nav epub:type="page-list">` or, when the NCX is the TOC that gets parsed, its `<pageList>`.
Absent when the book declares no page list (or none of its entries point into the spine). Read
only by the reader menu's **Go to Page**: the count on book open, then single records by seek.
Owned by `lib/Epub/Epub/PageList.cpp`.

### Version 1

```c++
struct PageListBin {
    u8  version;   // 1
    u8  reserved;  // 0
    u16 count;     // little-endian
    struct {
        char label[12];   // NUL padded UTF-8 page label as printed: "12", "xiv"
        u16  spineIndex;  // little-endian
        char anchor[50];  // NUL padded fragment id; empty = top of the spine item
    } entries[count];     // 64 bytes each, in document order
};
```

Entries with an empty label, a target outside the spine or an anchor of 50 bytes or more are
skipped; a label longer than 11 bytes is cut on a UTF-8 boundary; at most 8000 entries are kept.
