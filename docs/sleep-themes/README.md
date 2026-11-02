# Sleep themes

The sleep screen is always a theme: an HTML/CSS page laid out at the screen size and
compiled in a browser into files the device draws at every sleep.

- **Get themes:** the theme store at
  [InsiderPhD/katiepoint-sleep-themes](https://github.com/InsiderPhD/katiepoint-sleep-themes),
  installed from the device's sleep theme picker in Settings or from the web UI's
  **Sleep Themes** page. The replacements for the old sleep screens (Dark, Light, Cover,
  Custom, Blank, Page Overlay) are store themes too.
- **Make or change one:** the Sleep Theme Bench,
  <https://insiderphd.github.io/katiepoint-sleep-themes/bench/>, a drag-and-drop editor
  whose "Start from" list holds every store theme. Save the zip and install it on the
  **Sleep Themes** page, or open a pull request on the store repo to share it.

This page is the reference for what a theme can do. The binary format the compiler writes
is in [file-formats.md](../file-formats.md) (`theme.bin`).

## How a theme becomes a sleep screen

The compiler (the device's **Sleep Themes** page, `src/network/html/SleepThemesPage.html`)
lays the page out at the screen size and:

- bakes everything static (shapes, art, headings, any CSS) into a 1-bit background,
  `bg.bmp`;
- keeps elements marked `data-var` / `data-template` live: their boxes, fonts and
  transforms go into `theme.bin`, and the device fills them with your current values every
  time it sleeps;
- writes one `fN.cpfont` per live font and size, holding only the characters needed.

The store repo runs this same page headlessly (its `tools/compile.mjs`, with a copy of the
page refreshed by `tools/sync-compiler.sh`), so a store build and a hand-compiled theme are
identical. Rebuild the store after changing the compiler or the format.

## Values

The page's "Writing a theme" section lists the attributes, and the table under it lists
every value (`stats.*`, `book.*`, `recent.*`, `sleep.image`, `time.*`, `device.*`), from
`src/sleeptheme/SleepThemeVars.h`.

- `recent.1` … `recent.10` are the recently opened books, most recent first (the home
  screen's recents list, skipping books whose file is gone), each with `.title`,
  `.author`, `.progress` and `.cover`; `recent.count` says how many there are. A recent
  cover comes from images already cached on the SD card: the book's full cover if one has
  been made, otherwise the largest home-screen thumbnail, which looks soft when drawn much
  bigger than the home screen draws it.
- `sleep.image` is a random BMP from `/sleep` (or `/.sleep`, or `/sleep.bmp` on the card
  root): what the Custom sleep screen used to show.
- `time.now` is the moment the device went to sleep. The screen doesn't change while it
  sleeps, so show it as "Put down at 22:41" or "since 22:41", never as a clock. It is empty
  until the device knows the time (network time since boot, or a clock chip), so pair it
  with `data-hide-if-empty`, or `{{time.now ? … : …}}` for wording that needs it.
- `stats.week_met_1` … `_7` are Monday to Sunday of this week, `100` on a day the daily
  goal was met and `0` otherwise. Test them with `> 0`.

## Images

`book.cover`, `recent.N.cover` and `sleep.image` go on an `<img>` (or a `<div>`).
`object-fit` says how the image meets the box:

| `object-fit` | Bench | Device |
|---|---|---|
| `contain` | Whole cover | the whole image, as large as fits; the rest of the box stays white |
| `cover` | Zoom | scaled until it fills the box both ways, centred, the overflow cut off |
| anything else (`fill`) | Stretch | squeezed to the box's shape (greyscale images can't stretch and zoom instead) |

`data-filter` picks how it is drawn:

| `data-filter` | Device |
|---|---|
| `dither` (default) | greys as dot patterns |
| `grey` | the panel's four greys; best for covers and photos |
| `lighter`, `darker` | dithered, lifted or darkened first |
| `bw` | hard black and white, no dots; crisp for type and line art |
| `invert` | dithered negative |

`grey` needs an untransformed image in a whole-screen theme; rotated or skewed images,
and every image in an overlay theme, are dithered instead. Keep live text off grey
images.

## Overlay themes

`<html data-kp-overlay>` draws the theme over the screen that was showing (usually the
page you were reading) instead of a blank one. Leave the `html`/`body` background unset:
only what the theme paints covers the page, and the compiler writes `mask.bmp` alongside
`bg.bmp` to say which pixels those are.

## Expressions and icons

Anything inside `{{ }}` can be an expression, worked out by the device at every sleep:

- `{{stats.streak}} {{stats.streak == 1 ? "day" : "days"}}`
- `{{round(stats.week_minutes / 60, 1)}} hours this week`
- `{{book.title || "Nothing open"}}`, `{{upper(book.author)}}`, `{{date(time.now, "dd.MM.yyyy")}}`
- `{{stats.goal_met ? "Goal met" : stats.goal_left_time + " to go"}}` (`+` joins text)

Operators: `+ - * / %`, `== != < <= > >=`, `&& || !` (JavaScript-style), `?:` and
parentheses. Functions: `round(x, digits)`, `floor`, `ceil`, `abs`, `min(a, b)`,
`max(a, b)`, `upper`, `lower`, `pad(n, width)`, `plural(n, "one", "many")` and
`date(time, "pattern")`. A missing value makes arithmetic empty, so `data-hide-if-empty`
hides the element.

`icon("name")` draws `icons/name.svg` from the theme folder as part of the text, at the
text's size and colour: `{{stats.week_met_1 > 0 ? icon("flame-filled") : icon("flame")}}`.
The compiler turns each icon into a glyph of the element's font, so the device only sees
text. The bench (and the store's build) writes the SVGs for you from Lucide, the set the
device's home and file icons come from; `icon("name-filled")` is the outline filled in.

Date patterns are Unicode: `yyyy yy MM M dd d HH H hh h mm ss a` (`MM` month, `mm` minutes).

## Layout

- The device draws the static background, then images, then any static art that sits in
  front of an image in the page (a card over a full-screen cover; the compiler finds it
  and stores it as `front.bmp`), then live text and bars. Live text and bars are always in
  front of static art, whatever the page order, so don't put art over them.
- Live text uses the fonts you declare with `@font-face`. The compiler includes Latin
  characters by default; `data-glyphs="digits"` (or `cjk`, `kana`, `greek`, `cyrillic`,
  `hangul`) changes the set. Use `digits` on big numbers: it keeps the font file small,
  and a large size with the full Latin set can exceed the 255 px glyph limit. A value with
  a character the theme font lacks is drawn in the device's UI font instead.
- A box is measured around its sample content. Wrapped text (`data-max-lines`) gets room
  for every line; single-line text that turns out wider grows according to its alignment.
  The extra lines go below the sample unless you set `data-valign`: with `bottom` (or
  `middle`) the box keeps room for all its lines in the layout, like a slide's text box,
  and shorter text sits at its bottom (or middle). Use it for a title above an author, so
  a two-line title grows upward instead of over the author.
- A live element's border and background are art, baked at the sample's size, unless it
  has `data-fit`: the device then draws them around the real value (a tag pill), up to the
  element's `max-width`. Give it `width: fit-content`, single-line text and no transform,
  and add `data-overflow="ellipsis"` so a value longer than `max-width` is cut. The device
  doesn't move other elements, so give each pill a line of its own, or put the pills in a
  `display: flex; flex-wrap: wrap` parent: the device then lines them up side by side and
  wraps them itself (`gap` spaces them, `data-max-lines` on the parent caps the rows). Year
  in Books' shelf pills use a wrapping row.
- `letter-spacing` works as in CSS, spaces included (firmware before this drew no extra
  space beside a space, so spaced capitals ran their words together).
- Glyphs and line heights are limited to 255 px, a theme can use at most 4 font/size
  combinations for live text, 64 live elements and 4096 bytes of templates.
