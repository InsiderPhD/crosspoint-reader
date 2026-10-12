#!/usr/bin/env python3
"""Generates test_inline_images_css.epub: on-page footnotes, inline images, publisher CSS,
headings / block font sizes. Deterministic output (fixed timestamps), no dependencies.

usage: gen_epub.py <out.epub>
"""
import struct
import sys
import zipfile
import zlib


# ---------------------------------------------------------------- tiny PNG writer
def png(width, height, pixel):
    """8-bit grayscale PNG; pixel(x, y) -> 0..255."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.append(max(0, min(255, int(pixel(x, y)))))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def disc(size):  # filled circle with a white tick mark: an "icon"
    c = (size - 1) / 2.0

    def px(x, y):
        d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
        if d > c:
            return 255
        if abs((x - c) - (y - c) * 0.6) < 1.2 and y > c * 0.6:
            return 255
        return 0
    return png(size, size, px)


def box(w, h, border=2):  # outlined box with a diagonal: "inline maths" stand-in
    def px(x, y):
        if x < border or y < border or x >= w - border or y >= h - border:
            return 0
        if abs(x * h - y * w) < w * 1.5:
            return 0
        if (x // 6) % 2 == 0 and h // 3 < y < h // 3 + 3:
            return 90
        return 255
    return png(w, h, px)


def dropcap(w, h):  # a blocky serif "T" on a light ground with a frame
    def px(x, y):
        if x < 2 or y < 2 or x >= w - 2 or y >= h - 2:
            return 0
        if 8 <= y < 16 and 6 <= x < w - 6:
            return 0
        if w // 2 - 5 <= x < w // 2 + 5 and 16 <= y < h - 10:
            return 0
        if h - 12 <= y < h - 8 and w // 2 - 11 <= x < w // 2 + 11:
            return 0
        return 235
    return png(w, h, px)


def photo(w, h):  # gradient "photo", stays a block image
    def px(x, y):
        if x < 3 or y < 3 or x >= w - 3 or y >= h - 3:
            return 0
        return (x * 255 // w + y * 255 // h) // 2
    return png(w, h, px)


IMAGES = {
    "icon16.png": disc(16),
    "icon24.png": disc(24),
    "formula.png": box(120, 28),
    "tallformula.png": box(90, 40),
    "dropcap.png": dropcap(36, 42),
    "ornament.png": box(60, 20),
    "photo.png": photo(300, 200),
}

LOREM = ("The quick brown fox jumps over the lazy dog while the river keeps its slow course to the sea. "
         "Nobody in the village could say when the lighthouse had last been lit, only that it had. ")

CSS = """
body { margin: 0; }
p { margin: 0; text-indent: 1.2em; }
p.first { text-indent: 0; }
p.center { text-align: center; text-indent: 0; }
img.inl { display: inline; }
img.blk { display: block; }
.note { font-size: 0.8em; }
.big { font-size: 1.5em; }
.loose { line-height: 2; }
.tight { line-height: 1; }
.newpage { page-break-before: always; }
.endpage { page-break-after: always; }
.verse { white-space: pre-wrap; text-indent: 0; margin-left: 1.5em; }
.boxed { border: 1px solid black; padding: 0.4em; margin: 0.6em 0; text-indent: 0; }
.rules { border-top: 2px solid; border-bottom: 2px solid; padding: 0.3em 0; margin: 0.6em 0; text-indent: 0; }
.shaded { background-color: #dddddd; padding: 0.4em; margin: 0.6em 0; text-indent: 0; }
.dashed { border: 2px dashed; padding: 0.4em; margin: 0.6em 0; }
div.quote p { font-style: italic; margin-left: 2em; text-indent: 0; }
div.quote > p.attrib { text-align: right; font-style: normal; }
h2.sub { font-size: 1em; font-style: italic; }
"""


def page(title, body):
    return ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>%s</title>"
            "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head>\n<body>\n%s\n</body></html>\n"
            % (title, body))


FOOT = page("Footnotes", """
<h1>Footnotes</h1>
<p class="first">%(l)sA first remark needs a note<a href="#fn1">1</a> and the sentence goes on for a while after it so
the reference sits mid-paragraph. %(l)s</p>
<p>%(l)sA second remark<a href="#fn2">2</a> follows close behind. %(l)s</p>
<p>%(l)s</p>
<p id="fn1">1. The first note is long enough to wrap onto a second and a third line at the bottom of the page, which
is what makes the spacing of the footnote area visible next to the body text above it.</p>
<p id="fn2">2. A shorter second note, still more than one line wide when it is drawn under the rule.</p>
""" % {"l": LOREM})

INLINE = page("Inline images", """
<h1>Inline images</h1>
<p class="first">An icon <img src="icon16.png" alt="tick"/> sits inside this sentence, and a larger one
<img src="icon24.png" alt="tick"/> a little further on. %(l)s</p>
<p>Inline maths such as <img src="formula.png" alt="x over y"/> flows with the words, and so does a taller
expression <img src="tallformula.png" alt="sum"/> which makes its line taller. %(l)s</p>
<p class="first"><img src="dropcap.png" alt="T"/>his paragraph opens with a drop-cap glyph image glued to the rest
of its first word. %(l)s</p>
<p>Glued to a word:<img src="icon16.png" alt=""/>, with punctuation after it. Three in a row:
<img src="icon16.png" alt=""/><img src="icon16.png" alt=""/><img src="icon16.png" alt=""/> done.</p>
<p class="center"><img src="ornament.png" alt="ornament"/></p>
<p>The ornament above stands alone in its paragraph, so it stays a centred block image. %(l)s</p>
<p>Forced block: <img class="blk" src="icon24.png" alt=""/> text after the block image.</p>
<p>A large picture never goes inline: <img src="photo.png" alt="photo"/> and the text carries on below it.</p>
<p>Forced inline by CSS: <img class="inl" src="tallformula.png" alt=""/> end.</p>
<table><tr><td>cell <img src="icon16.png" alt=""/> text</td><td>plain</td></tr></table>
<p>After the table.</p>
""" % {"l": LOREM})

STYLES = page("Publisher CSS", """
<h1>Publisher CSS</h1>
<p class="first loose">Loose line-height. %(l)s</p>
<p class="tight">Tight line-height. %(l)s</p>
<p class="boxed">A boxed paragraph with a solid border all round. %(l)s</p>
<p class="rules">Rules above and below only.</p>
<p class="shaded">A shaded aside. %(l)s</p>
<div class="dashed"><p class="first">A dashed box holding two paragraphs.</p><p>Second paragraph in the box.</p></div>
<p class="verse">Preserved   spacing
    and line breaks
  in a verse block.</p>
<pre>def greet(name):
    return "Hello, " + name

print(greet("reader"))</pre>
<div class="quote"><p>A contextual rule styles this quotation in italics with a left margin.</p>
<p class="attrib">Attribution, right aligned by a child rule</p></div>
<p>Text before a forced page break.</p>
<p class="newpage">This paragraph starts a new page (page-break-before).</p>
<p class="endpage">This one ends its page (page-break-after).</p>
<p>And this is on the page after.</p>
""" % {"l": LOREM})

HEADS = page("Headings", """
<h1>Heading level one</h1>
<p class="first">Body text under the first heading. %(l)s</p>
<h2>Heading level two</h2>
<p class="first">Body text. %(l)s</p>
<h3>Heading level three</h3>
<p class="first">Body text.</p>
<h4>Heading level four</h4>
<h5>Heading level five</h5>
<h6>Heading level six</h6>
<h2 class="sub">A level-two heading sized back to body text by CSS</h2>
<p class="big">A paragraph at 1.5em. %(l)s</p>
<p class="note">A paragraph at 0.8em, the size publishers use for notes and credits. %(l)s</p>
<p>Back to body size.</p>
""" % {"l": LOREM})

CHAPTERS = [("foot.xhtml", "Footnotes", FOOT), ("inline.xhtml", "Inline images", INLINE),
            ("styles.xhtml", "Publisher CSS", STYLES), ("heads.xhtml", "Headings", HEADS)]


def build(out):
    manifest = "".join('<item id="c%d" href="%s" media-type="application/xhtml+xml"/>' % (i, n)
                       for i, (n, _, _) in enumerate(CHAPTERS))
    manifest += "".join('<item id="i%d" href="%s" media-type="image/png"/>' % (i, n)
                        for i, n in enumerate(sorted(IMAGES)))
    spine = "".join('<itemref idref="c%d"/>' % i for i in range(len(CHAPTERS)))
    opf = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
           "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">"
           "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">test-inline-images-css"
           "</dc:identifier><dc:title>Inline Images and CSS Test</dc:title><dc:creator>KatiePoint</dc:creator>"
           "<dc:language>en</dc:language></metadata><manifest>"
           "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
           "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>%s</manifest><spine>%s</spine></package>"
           % (manifest, spine))
    nav = page("Contents", "<nav xmlns:epub=\"http://www.idpf.org/2007/ops\" epub:type=\"toc\"><ol>%s</ol></nav>" %
               "".join('<li><a href="%s">%s</a></li>' % (n, t) for n, t, _ in CHAPTERS))
    container = ("<?xml version=\"1.0\"?><container version=\"1.0\" "
                 "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
                 "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
                 "</rootfiles></container>")
    files = [("mimetype", b"application/epub+zip", zipfile.ZIP_STORED),
             ("META-INF/container.xml", container.encode(), zipfile.ZIP_DEFLATED),
             ("OEBPS/content.opf", opf.encode(), zipfile.ZIP_DEFLATED),
             ("OEBPS/nav.xhtml", nav.encode(), zipfile.ZIP_DEFLATED),
             ("OEBPS/style.css", CSS.encode(), zipfile.ZIP_DEFLATED)]
    files += [("OEBPS/" + n, body.encode(), zipfile.ZIP_DEFLATED) for n, _, body in CHAPTERS]
    files += [("OEBPS/" + n, IMAGES[n], zipfile.ZIP_STORED) for n in sorted(IMAGES)]
    with zipfile.ZipFile(out, "w") as z:
        for name, data, method in files:
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 12, 0, 0, 0))
            info.compress_type = method
            z.writestr(info, data)


if __name__ == "__main__":
    build(sys.argv[1])
