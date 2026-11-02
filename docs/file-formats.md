# File Formats

## Fork namespace (`book.bin`, `section.bin`, TXT `index.bin`)

Upstream CrossPoint and CrossInk keep walking their own `book.bin` / `section.bin`
version numbers with layouts that differ from ours, so a bare version byte cannot tell
a KatiePoint file from another firmware's file of the same number (a card swapped between
firmwares would then be parsed against the wrong layout). Each of these three files
therefore starts with a 4-byte fork magic, written as a little-endian `u32`, followed by
the `u8` version:

| File | Magic bytes | `u32` constant | Version constant |
|------|-------------|----------------|------------------|
| `book.bin` | `FF 'K' 'P' 'B'` | `0x42504BFF` | `BOOK_CACHE_VERSION` (from v14) |
| `sections/<n>.bin` | `FF 'K' 'P' 'S'` | `0x53504BFF` | `SECTION_FILE_VERSION` (from v50) |
| TXT `index.bin` | `FF 'K' 'P' 'T'` | `0x54504BFF` | `CACHE_VERSION` in `TxtReaderActivity.cpp` (from v5) |

Why this cannot collide in either direction:

- **Upstream CrossPoint** starts `book.bin` and `section.bin` with a bare `u8` version.
  Its section versions climb from 28 while its partial-build marker counts *down* from
  `0xFE`, so byte 0 is never `0xFF`; our reader rejects its files on the magic, and its
  reader rejects ours on the version byte. (Were it ever to reach 255, the next field it
  checks, `fontId`, would read `'K' 'P' 'S' <version>` and still mismatch.)
- **CrossInk** uses the same idea with its own magics, `FF 'C' 'X' 'B'` / `FF 'C' 'X' 'S'`,
  and the plain `"TXTI"` magic for its TXT index. Bytes 1-3 differ from ours, so each side
  rejects the other's file on the magic regardless of version.
- **Older KatiePoint builds** wrote byte 0 = version (13 / 48) and `"TXTI"`; they reject the
  new files on byte 0 / the magic, and the new build rejects theirs the same way.

A rejected file is rebuilt, never parsed. Never change these magics; bump the version byte
after them as before. `progress.bin` is not part of this scheme and keeps its own layout.
The CSS rule cache (`css_rules.cache`) is separately namespaced by bit 7 of its version byte.

## `book.bin`

> **Current version: 14** (`BOOK_CACHE_VERSION` in `lib/Epub/Epub/BookMetadataCache.cpp`).
> v14 prepends the fork magic `FF 'K' 'P' 'B'` (see *Fork namespace* above) before the version
> byte; everything after it, including the absolute offsets in the LUTs, is laid out as before
> (offsets simply include the 4 extra bytes).
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

> **Current version: 50** (`SECTION_FILE_VERSION` in `lib/Epub/Epub/Section.cpp`).
> v50 prepends the fork magic `FF 'K' 'P' 'S'` (see *Fork namespace* above) before the version
> byte; the rest of the header and the body are unchanged (header offsets shift by 4).
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
> The v48 header also gains `layoutFeatures u8` after `paragraphIndent` and before
> `pageCount` (bit 0: the build lays out headings and CSS-sized blocks in other sizes of
> the reader font, PSRAM boards only), compared on load like the render settings, so a
> card moved between an X4 and an X4C rebuilds. Sized lines use the existing
> `fontOverride` in each TextBlock; nothing else in the payload changes. On those builds
> the CSS cache version byte has bit 7 set (`0x88`): each rule carries one more length,
> `fontSize` (float + unit), after the border bytes, flagged by defined bit 30.
> v49 content-based reading positions (upstream #2805): the header gains a fifth trailing
> `u32 visibleTextLutOffset` after `listItemLutOffset` (so `pageCount` and the four older
> offsets each move 4 bytes earlier relative to the header end), and the file gains a
> visible-text LUT at that offset: `u32 visibleTextOffset[pageCount]`, the zero-based Unicode
> codepoint offset in the chapter's visible `<body>` text where each page starts
> (non-decreasing; page 0 is always 0). Visible text is the raw character data inside
> `<body>`, uncollapsed whitespace included, excluding `<head>`, `<style>`, `<script>`,
> `<title>` and `<rp>` subtrees and synthetic text such as image alt fallbacks, counted the
> same way upstream CrossPoint counts it so offsets are portable between devices. Those five
> elements are also no longer rendered when they appear inside `<body>`.
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

## Settings: `katiepoint-settings.json` and `settings.json`

KatiePoint's settings live in `/.crosspoint/katiepoint-settings.json` (JSON object, one key per
setting, written by `JsonSettingsIO::serializeSettings`). Saves go to `.tmp`, the previous file is
parked as `.bak` (kept as the last known good copy) and the scratch file renamed in.

`/.crosspoint/settings.json` belongs to upstream CrossPoint (and older KatiePoint builds, which
used it as their own file). Upstream rebuilds it from its own key list on every boot, which is why
KatiePoint no longer keeps its settings there. CrossInk uses `crossink-settings.json` for the same
reason.

**Load order** (`CrossPointSettings::loadFromFile`):

1. `katiepoint-settings.json`, after recovering an interrupted save from `.tmp` / `.bak`. If any of
   the three exists this file is authoritative: when damaged, its `.bak` is tried and otherwise
   defaults are used. `settings.json` is never imported over it.
2. Otherwise `settings.json` (or its `.tmp` / `.bak`) is imported once and our file saved straight
   away. `settings.json` is not moved or deleted.
   - **Upstream file**: it contains a key KatiePoint has never written (`lineHeight`,
     `sleepTimeoutMinutes`, `pluginsEnabled`, `paragraphIndentSpaces`, `pageTurnGesture`,
     `libraryUseMetadata`, `screenInverted`, `focusReadingEnabled`, `readerMenuStyle`). It is
     translated by `SettingsCompat::importUpstream` (table below); unmapped settings keep our
     defaults. Its `language` code sets the UI language.
   - **Otherwise** it is an older KatiePoint build's own file and loads as-is.
3. Otherwise legacy `settings.bin` (renamed to `settings.bin.bak` once migrated).

**Compatibility copy.** After every successful save the shared settings are written into
`settings.json` in upstream's key names and numbering (`SettingsCompat::exportShared`):

- An upstream `settings.json` is read (capped at 16 KB), every key we don't own is kept, and each
  shared key is replaced only when our value differs from what that file's own value translates to
  and back. Upstream's exact value survives for settings not changed here, including values with no
  KatiePoint equivalent.
- A KatiePoint-format `settings.json` (older build) is replaced by the shared keys alone.
- The write is atomic (`settings.json.tmp`, then renamed in). It is skipped when the content is
  unchanged, or when the shared payload hash matches the last one written this session. A read,
  parse or allocation failure only logs: it never fails our own save.
- `lineHeight` and `sleepTimeoutMinutes` are always written, so the result reads back as upstream.

**Language.** `language.bin` stays the source of truth. If it is missing, `language.bin.bak` is
read; CrossInk renames `language.bin` to that. Both settings files also carry `"language": "<code>"`,
which is used when neither binary file exists.

### Translation table (KatiePoint <-> CrossPoint 2.0)

Same name, value and range on both sides: `hideBatteryPercentage`, `fadingFix`, `wordSpacing`,
`screenMargin`, `paragraphAlignment`, `embeddedStyle`, `hyphenationEnabled`, `textAntiAliasing`,
`extraParagraphSpacing`, `orientation`, `frontButtonFollowOrientation`, `frontButtonBack/Confirm/Left/Right`,
`tiltPageTurn`, `showHiddenFiles`, `statusBarProgressBar`, `statusBarProgressBarThickness`,
`clockUtcOffsetQ`, `clockFormat`, `clockTimezone` (same 104-zone table), `clockDst`,
`frontlightWarmth`, `dictionaryName`, `sdFontFamilyName`, `language`.

| KatiePoint | CrossPoint 2.0 | Mapping |
| --- | --- | --- |
| `darkMode` | `screenInverted` | same 0/1 |
| `bionicReading` | `focusReadingEnabled` | same 0/1 |
| `sleepTheme` | `sleepScreen` | dark 0, light 1, custom 2, cover-crop 3 (+ `sleepScreenCoverMode` 1), cover-or-custom 4, blank 5, page-overlay 7. Downloaded themes: not exported. Upstream 8 (Cover + Transparent): not imported |
| `seamlessSleepScreen` | `quickResumeSleepScreen`, `sleepScreen` | Never 0 / After timeout 1 <-> quickResume 0 / 1; Always <-> `sleepScreen` 6 (Quick Resume) |
| `refreshFrequency` 0..4 | `refreshFrequency` 0..5 | same; upstream 5 (Never) imports as 4 (30 pages) |
| `uiTheme` | `uiTheme` | 0..2 same; our retired 3 exports as 2; Carousel / Dashboard / Bookshelf not exported; upstream RoundedRaff 3 / Cover Grid 4 not imported |
| `fontFamily` | `fontFamily` | upstream always 1 (Noto Sans) unless an SD family is set; our built-in OpenDyslexic <-> upstream legacy 2; Bookerly / Inter / Monospace export as 1 |
| `fontSize` 0..3 | `fontSize` (pt) | slot i <-> 12 + 2i pt; other point sizes snap to the nearest slot; legacy 0..3 import as-is |
| `lineSpacing` Tight / Normal / Wide | `lineHeight` 0..6 | i <-> i + 1 (upstream's own legacy migration); upstream 0 -> Tight, 4..6 -> Wide |
| `characterSpacing` 0..6 (half px, 3 = 0) | `characterSpacing` 0..4 (whole px, 2 = 0) | import {0,1,3,5,6}; export {0,1,1,2,3,3,4} (round trip exact for upstream values) |
| `paragraphIndent` (0 book, 1 none, 2..5 = 1..4 spaces) | `paragraphIndentSpaces` 0..5 | n spaces <-> n + 1; upstream 5 -> 4 spaces; "book" not exported |
| `imageRendering` + `imageGrayscale` | `imageRendering` 0..3 | Display + grayscale off <-> 3 (Display B/W); others same |
| `sleepTimeout` (1/5/10/15/30 min) | `sleepTimeoutMinutes` | minutes; import snaps to the nearest (31 = never -> 30) |
| `finishedBookFolder` | `moveFinishedToReadFolder` | Off 0 / Read 1 same; Completed not exported |
| `frontlightBrightness` (0 = off), `frontlightLastBrightness` | `frontlightBrightness`, `frontlightOn` | off <-> `frontlightOn` 0 with the last level as brightness |
| `statusBar*Pos` (bottom bar) | `statusBarBattery`, `statusBarTitle`, `statusBarBookProgressPercentage`, `statusBarChapterPageCount`, `statusBarClock` | shown <-> 1; title Chapter wins over Book; clock Left <-> 2, other positions <-> 1 (Right) |
| `xtcStatusBarMode` 0..3 | `xtcStatusBarMode` 0..2 | Both exports as Bottom |
| `showClockOutsideReader` | `clockShowHeader` | same 0/1 |
| `readerShortPressSideUp/Down`, `readerLongPressSideUp/Down` | `sideButtonLayout`, `longPressButtonBehavior` | layouts Prev/Next, Next/Prev, Disabled, Next/Next, Prev/Prev; long press Off / chapter skip / rotate |
| `readerShortPressPower` | `shortPwrBtn` | None 0 (also Sleep 1 on import), Page forward 2, Refresh 3, Footnotes 4, Clipping 6, Bookmark 7, Sync 8, Menu 9 (also Confirm 5 on import) |
| `readerLongPressConfirm` | `longPressMenuFunction` | Sync 0, None 1, Bookmark 2, Dictionary 3, Menu 4, Clipping 5, Footnotes 6 |

Importing an upstream file sets `readerActionsMigrated`, so the legacy reader-action migration
does not overwrite the translated actions. Everything else is KatiePoint-only or upstream-only and
is not translated.

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

## Clippings: `/.crosspoint/clippings/kp_epub_<hash>.bin`

Saved clippings for one book, keyed by `std::hash` of the book path. Owned by
`src/ClippingStore.cpp`. Strings are `u32` length + bytes (`lib/Serialization`).

The folder is shared with upstream CrossPoint (`epub_<std::hash>.bin`, the same names
our older firmware used) and CrossInk (`epub_<crc32>.bin`). Both start their files with a
bare version byte whose numbers overlap ours with different record layouts, so our files
carry a `"KPCL"` magic and a `kp_` name prefix. Neither firmware writes either, and both
reject a file whose first byte is `'K'`.

### Version 5

```c++
struct ClippingsBin {
    char   magic[4]; // "KPCL"
    u8     version;  // 5 (1..4 also load: a moved untagged file keeps its record version)
    u16    count;    // <= CLIPPING_MAX_PER_BOOK (64)
    string title;
    string author;
    string bookPath;
    struct {
        u16  spineIndex, startPage, endPage, pageCount;
        u16  startWordIndex, endWordIndex, wordCount, paragraphIndex;  // 0xFFFF = none
        u32  timestamp;        // UTC epoch seconds, 0 = clock unset
        u32  bookFusionId;     // v2+; 0 = not pushed
        u32  bookFusionStart, bookFusionEnd;  // v3+
        u8   bookFusionFlags;  // v3+; BF_* bits
        u8   syncFlags;        // v4; 0x01 pending upload, 0x02 foreign layout anchor,
                               //     0x04 content-offset lookup reached a verdict (v5)
        u64  syncRevision;     // v4; server revision, 0 = never synced
        string id;             // v4; clipping-sync id, empty until first sync (<= 64 chars)
        u32  startOffset, endOffset;  // v5; chapter visible codepoints [start, end), 0xFFFFFFFF = unknown
        char chapterTitle[48]; // NUL padded
        string text;           // <= 512 bytes
    } clippings[count];
};
```

Missing fields in older record versions take their defaults (every clip pending upload,
no sync id, no content offsets).

### Untagged files: `epub_<hash>.bin`

Firmware before the magic wrote the same records without `magic`, under the name upstream
also uses. When a book has no `kp_` file, the untagged file at its name is identified like
this, and anything that cannot be attributed with certainty is skipped, never misparsed:

| version | origin | handling |
|---|---|---|
| 5 | ours (nobody else wrote a v5) | loaded; rewritten as `kp_…` on the next save, untagged file removed |
| 1 | identical bytes in ours and upstream's | loaded read-only; the untagged file is left in place |
| 2-4 | records parsed both as our layout and as upstream 2.0.0's; only the one that ends exactly at EOF wins | ours: as v5. Upstream: imported read-only (below). Both or neither: skipped |

Upstream 2.0.0 records (imported, never written): the common `u16`x8 + `u32 timestamp`,
then `u32 layoutSignature` (v3+), then `u32 startOffset, endOffset; u64 syncRevision;
u8 pendingUpload; char id[65]` (v4), then `char chapterTitle[48]`, `u16 textLength` (<= 4096)
and the text. Imported clips are marked foreign-anchor (listed, not highlighted), text is
cut to 512 bytes on a UTF-8 boundary and only the first 64 clips are kept. Upstream's file
is shadowed, not changed, once a `kp_` file exists for the book.

When listing clipped books, an untagged name must also equal `std::hash` of the path in its
header, which rules out CrossInk's crc32 names; it is ignored when a `kp_` file for the same
book exists. Journals, upstream's `.bak` / `.tmp` sidecars and other names are skipped.

### Deletion journal: `kp_epub_<hash>.bin.deleted`

Ids of synced clips deleted on this device, waiting to be sent to the clipping-sync
server: back-to-back 65-byte NUL-padded records. A torn trailing record is trimmed on the
next open; the file is removed once the server has acknowledged every entry. An untagged
file's journal (`epub_<hash>.bin.deleted`) is renamed along with it when it migrates.

## Sleep themes: `/sleep-themes/<name>/`

Written by the browser compiler on the web UI's Sleep Themes page
(`src/network/html/SleepThemesPage.html`), read by `src/sleeptheme/SleepThemeRenderer.cpp`.
A theme folder holds:

- `theme.bin`: the layout below.
- `bg.bmp`: 1-bit BMP, exactly `screenWidth` x `screenHeight`, everything static baked in.
- `mask.bmp` (overlay themes only): 1-bit BMP, same size, black where `bg.bmp` is painted.
- `front.bmp` (flag 0x04): 1-bit BMP, same size, black where `bg.bmp` is drawn again after
  the images, because that static art sits in front of one. In grey passes those pixels are
  left as the BW base drew them.
  Elsewhere the screen that was showing when the device went to sleep stays.
- `f0.cpfont` .. `f3.cpfont`: v4 `.cpfont` files (no kerning or ligatures) holding the
  glyphs live text can need, one file per font family + pixel size. The format is 2-bit,
  but the compiler only writes levels 0 and 3, cut at 50% coverage: themes draw in black
  and white, where any non-zero level is ink, so this keeps live text the weight the
  preview shows.
- `source/`: the uploaded HTML folder, so the theme can be recompiled.

### `theme.bin` version 1

Little-endian, fixed-size records (see `src/sleeptheme/SleepThemeFormat.h`).

```c++
struct ThemeBin {
    char magic[8];           // "KPTHEME\0"
    u16  version;            // 1
    u16  screenWidth;        // logical size the theme was laid out at
    u16  screenHeight;
    u8   orientation;        // GfxRenderer::Orientation
    u8   elementCount;       // <= 64
    u8   fontCount;          // <= 4
    u8   flags;              // 0x01 cover elements store their template (older themes: 0,
                             //      and every cover shows the open book)
                             // 0x02 overlay: draw bg.bmp through mask.bmp, clear nothing
                             // 0x04 front: draw bg.bmp through front.bmp after the images
    u16  stringTableSize;    // <= 4096
    u8   reserved[12];
    struct {                 // 48 bytes
        u8   kind;           // 0 text, 1 cover (book.cover or recent.N.cover), 2 bar,
                             // 3 box: see below
        u8   fontIndex;      // fN.cpfont
        u8   fontStyle;      // 0 regular, 1 bold, 2 italic, 3 bold italic
        u8   align;          // 0 left, 1 centre, 2 right
        u8   flags;          // 0x01 white ink, 0x02 hide if a value is empty, 0x04 transformed,
                             // 0x08 ellipsis, 0x10 contain (cover), 0x20 opaque (cover),
                             // 0x40 zoom: fill the box, crop the overflow (cover),
                             // 0x80 flow (box): placed in a wrapping row, see below
        u8   textTransform;  // 0 none, 1 upper, 2 lower, 3 capitalize
        u8   maxLines;       // 0/1 single line; wrapped text is clamped to 8
        i8   digitIndex;     // >= 0: only the nth digit from the right
        i8   tracking;       // letter-spacing, half-pixel steps
        u8   verticalAlign;  // (text) 0 top, 1 middle, 2 bottom: where the lines sit in the box
                             //   when they are fewer/shorter than it (older themes: 0)
        u16  width, height;  // content box in the element's own coordinates
        u16  templateOffset; // into the string table; 0xFFFF none (a cover with none is book.cover)
        i32  matrix[6];      // a b c d e f, 16.16: screen = (a*x + c*y + e, b*x + d*y + f)
        u16  lineHeight;     // pixels between baselines, 0 = font default
        u16  barMax;         // (bar) the value that fills it; 0 = 100, a percentage
        u8   coverFilter;    // (cover) 0 dither, 1 lighter, 2 darker, 3 black and white (no dither),
                             //   4 inverted, 5 grey: the panel's four greys, drawn in grey passes
                             //   after the BW paint (untransformed, non-overlay only; else 0)
                             //   (older themes: 0)
        u8   reserved[3];
    } elements[elementCount];
    char strings[stringTableSize];  // NUL-terminated templates: "Day {{stats.streak:000}}"
                                    // or expressions: {{stats.streak == 1 ? "day" : "days"}}
                                    // (src/sleeptheme/SleepThemeExpr.h). icon("x") calls are
                                    // already replaced by private-use characters (U+E000 up)
                                    // whose glyphs the compiler added to the element's font.
};
```

A **box** element (kind 3, from `data-fit`) always comes just before the text element it
belongs to, and is drawn under it: that text's border and background, as wide as the
text's live value plus the insets and never wider than the box. It reuses the text fields:
`width`/`height`/`matrix` are the border box (a translation only), `fontIndex` the border
width, `fontStyle` the corner radius, `align` which side stays put (the text's alignment),
`textTransform`/`maxLines` the left/right insets (border plus padding), `verticalAlign` the
fill (0 none, 1 black, 2 white) and flag 0x01 a white border. Firmware without it skips the
element, so the text draws without its pill.

With flag 0x80 (flow: the pill's parent is a `flex-wrap: wrap` row) the device ignores the
box's position and lines the group's pills up itself, left to right with the column gap,
starting a new row when the next one doesn't fit, and draws the text with its box (moved as
far as the box moved). Hidden pills take no room. More fields are reused: `digitIndex` is
the group id (u8, from 1), `tracking` the most rows (u8, 0 no limit; the parent's
`data-max-lines`), `templateOffset` the row width, `lineHeight`/`barMax` the rows' left x
and top y, `coverFilter`/`reserved[0]` the column/row gap. Firmware without flow draws each
pill where the compiler measured it: fitted, but one per row.

A store install also writes `store.ver` into the theme folder: the build's `version` from the
store catalog (a short hash of its files). The store screens offer an update when it differs
from the catalog's, or is missing.

Template names are the ones `GET /api/sleep-theme/vars` lists (`src/sleeptheme/SleepThemeVars.h`).
A version or screen-size mismatch makes the device fall back to the default sleep screen.
