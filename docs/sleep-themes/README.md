# Sleep themes

A sleep theme is an HTML/CSS page laid out at the screen size. On the device's web UI,
**Sleep Themes** compiles it in your browser:

- everything static (shapes, art, headings, any CSS) is baked into a black-and-white
  background;
- elements marked `data-var` / `data-template` stay live, and the device fills them with
  your current values every time it sleeps.

The page's "Writing a theme" section lists the attributes, and the table under it lists
every value (`stats.*`, `book.*`, `recent.*`, `time.now`, `device.*`).

`recent.1` … `recent.10` are the recently opened books, most recent first (the home
screen's recents list, skipping books whose file is gone), each with `.title`, `.author`,
`.progress` and `.cover`; `recent.count` says how many there are. A recent cover is drawn
from images already cached on the SD card: the book's full cover if one has been made
(it has been a Cover sleep screen), otherwise the largest home-screen thumbnail, which
looks soft when drawn much bigger than the home screen draws it.

## Editor

`bench.html` (Sleep Theme Bench) is a drag-and-drop editor for themes: open it in desktop
Chrome or Firefox, place values, text, shapes, covers and icons on the screen, check the
result against the device's limits, and save a zip to install on the Sleep Themes page.
It keeps `theme.html` as the source, so you can switch to the code at any time.

## Examples

- `departures/`: flip-board tiles (`data-digit`) for hours read, streak and minutes this
  week, plus the current book's title.
- `finished-ticket/`: rotated cover, a rotated date stamp and a stat card, showing
  transforms on live content.

Both use Inter (SIL Open Font License, see `OFL-Inter.txt`), included in each folder. Pick
the folder on the Sleep Themes page.

## Expressions and icons

Anything inside `{{ }}` can be an expression, worked out by the device at every sleep:

- `{{stats.streak}} {{stats.streak == 1 ? "day" : "days"}}`
- `{{round(stats.week_minutes / 60, 1)}} hours this week`
- `{{book.title || "Nothing open"}}`, `{{upper(book.author)}}`, `{{date(time.now, "dd.MM.yyyy")}}`

Operators: `+ - * / %`, `== != < <= > >=`, `&& || !` (JavaScript-style), `?:` and
parentheses. Functions: `round(x, digits)`, `floor`, `ceil`, `abs`, `min`, `max`, `upper`,
`lower`, `pad(n, width)`, `plural(n, "one", "many")` and `date(time, "pattern")`. A missing
value makes arithmetic empty, so `data-hide-if-empty` hides the element.

`icon("name")` draws `icons/name.svg` from the theme folder as part of the text, at the
text's size and colour: `{{stats.week_met_1 ? icon("flame-filled") : icon("flame")}}`. The
compiler turns each icon into a glyph of the element's font, so the device only sees text.
The Sleep Theme Bench writes the SVGs for you from Lucide, the set the device's home and
file icons come from; `icon("name-filled")` is the outline filled in.

Date patterns are Unicode: `yyyy yy MM M dd d HH H hh h mm ss a` (`MM` month, `mm` minutes).

Installed themes are listed on the same page, where you can switch between them or
delete one.

## Things to know

- Live content is drawn on top of all static art. Don't put art over a live element.
- Live text uses the fonts you declare with `@font-face`. The compiler includes Latin
  characters by default; add `data-glyphs="cjk"` (or `kana`, `greek`, `cyrillic`,
  `hangul`, `digits`) when live text needs others. A value with a character the theme
  font lacks is drawn in the device's UI font instead.
- A box is measured around its sample content. Wrapped text (`data-max-lines`) gets room
  for every line; single-line text that turns out wider grows according to its alignment.
- Glyphs and line heights are limited to 255 px, and a theme can use at most 4
  font/size combinations for live text.
