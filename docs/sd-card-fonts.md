# SD Card Fonts

CrossPoint supports loading additional fonts from the SD card, including fonts
with extended Unicode coverage (CJK, Cyrillic, Greek, etc.).

## Installing Fonts

There are three ways to install fonts:

### Option 1: Download from device (recommended)

1. Connect your CrossPoint reader to WiFi
2. Go to **Settings > System > Download Fonts**
3. Browse available font families and tap to download
4. Downloaded fonts appear immediately in **Settings > Reader > Font Family**

### Option 2: Upload via web browser

1. Connect your CrossPoint reader to WiFi
2. Open the web interface in your browser (shown on the WiFi screen)
3. Navigate to the **Fonts** tab
4. Upload `.cpfont` files using the upload form

### Option 3: Manual SD card copy

1. Download font files from the
   [Releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases/tag/sd-fonts)
2. Copy font family folders to `/.crosspoint/fonts/` on your SD card:

       SD Card Root/
       └── .crosspoint/
           └── fonts/
               ├── Bookerly-SD/
               │   ├── Bookerly-SD_12.cpfont
               │   ├── Bookerly-SD_14.cpfont
               │   ├── Bookerly-SD_16.cpfont
               │   └── Bookerly-SD_18.cpfont
               └── ...

3. Insert the SD card and power on your CrossPoint reader

## Available Pre-Built Fonts

| Font | Best For | Languages |
|------|----------|-----------|
| Bookerly-SD | General reading | English, Western European |
| NotoSansExtended | Multi-script reading | European, Greek, Cyrillic, Georgian, Armenian, Ethiopic |
| NotoSansCJK | Chinese/Japanese/Korean | CJK + ASCII |

## Converting Custom Fonts

To convert your own TrueType/OpenType fonts:

### Prerequisites

    pip install freetype-py fonttools

### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### 16-level (4-bit) fonts

Add `--4bit` to either command to store 16 coverage levels per glyph pixel
instead of four (header flags `0x0002` instead of `0x0001`). With "16-Level
Text" on, the Xteink X3 and the Picco draw these at all sixteen tones; every
other board, and the X3 with the setting off, folds them onto the usual four.
A 4-bit family's glyph bitmaps are twice the size of a 2-bit one, so expect
twice the SD bytes and twice the glyph RAM per page; give it its own family
name (for example `MyFont16`) so it can sit beside the 2-bit build.
`--gamma 1.1` thickens light edge pixels slightly if the result looks thin.

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext --sizes 12,14,16,18 \
      --name MyFont16 --4bit --output-dir ./MyFont16/

`build-sd-fonts.py` takes the same `--4bit` / `--gamma` switches, or per
family `four_bit: true` and `gamma:` keys in `sd-fonts.yaml`.

### Available Unicode interval presets

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020-U+007E (Basic Latin) |
| `latin-ext` | European languages (Latin + Extended-A/B) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables |
| `builtin` | Matches built-in Bookerly coverage exactly |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

Install custom fonts via WiFi upload or manual SD card copy.

## TrueType / OpenType fonts (X4 Pro and X4C only)

Boards with PSRAM (X4 Pro, X4C) can also read with a `.ttf` or `.otf` file
directly, with no conversion step. The X3 and X4 have no PSRAM: they ignore
these files and keep using `.cpfont` fonts.

Copy the font to the SD card in either layout (`/fonts` or `/.fonts`):

    SD Card Root/
    └── fonts/
        ├── Literata.ttf                 one file = one family named "Literata"
        └── Georgia/                     a folder = one family named "Georgia"
            ├── Georgia-Regular.ttf
            ├── Georgia-Bold.ttf
            ├── Georgia-Italic.ttf
            └── Georgia-BoldItalic.ttf

The family then appears in **Settings > Reader > Font & Layout > Reader Font
Family**, next to the built-in and `.cpfont` families, and follows the same
**Reader Font Size** setting (Small / Medium / Large / X Large = 6 / 8 / 10 /
12 pt, the sizes of the built-in reader families).

- Only a regular file is required. A missing bold or italic is synthesised from
  the regular design; adding real Bold / Italic files replaces the synthesised
  style. In a folder the style of each file is read from the font itself
  (weight and italic flag), so file names do not matter.
- Limits: 3 MB per file, 4 MB per family, family names up to 31 characters. A
  file over the limit is not listed. Large CJK fonts will not fit: convert
  those to `.cpfont` instead.
- Rendering uses the stb_truetype rasteriser: no hinting, variable fonts are
  drawn at their default instance, kerning is read from the `kern` table or
  simple GPOS pair lookups, and `fi`/`fl`/`ff` ligatures need the font to map
  U+FB00-U+FB04. A font converted to `.cpfont` (FreeType, hinted) is sharper at
  small sizes.
- Only install font files you trust. stb_truetype does not validate font data;
  a corrupt file can crash the device when the book opens. If that happens,
  pick another font in Settings or remove the file from the card.

