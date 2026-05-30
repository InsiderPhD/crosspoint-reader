#!/usr/bin/env python3
"""Generate a rendering-test EPUB that exercises the CrossPoint renderer.

Covers: bold, italic, bold-italic, inline code, code block, footnotes
(noteref link + body element), and an embedded image.

Output: rendering-test.epub in the repo root.
"""
import os
import struct
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "rendering-test.epub")


def make_image(path):
    """Write a 300x200 PNG with a gradient + bars so dithering is visible."""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        raise SystemExit("Pillow is required: pip install pillow")
    w, h = 300, 200
    img = Image.new("L", (w, h), 255)
    px = img.load()
    # Horizontal gradient (tests grayscale dithering on e-ink)
    for x in range(w):
        for y in range(h // 2):
            px[x, y] = int(255 * x / w)
    d = ImageDraw.Draw(img)
    # Black/white test bars in the lower half
    for i in range(10):
        shade = 0 if i % 2 == 0 else 255
        d.rectangle([i * w // 10, h // 2, (i + 1) * w // 10, h], fill=shade)
    d.rectangle([0, 0, w - 1, h - 1], outline=0)
    d.text((10, 10), "IMG TEST", fill=0)
    img.save(path, "PNG")


CONTAINER_XML = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

CONTENT_OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">urn:uuid:crosspoint-render-test-0001</dc:identifier>
    <dc:title>CrossPoint Rendering Test</dc:title>
    <dc:creator>Test Harness</dc:creator>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch2" href="ch2.xhtml" media-type="application/xhtml+xml"/>
    <item id="img1" href="images/test.png" media-type="image/png"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="ch1"/>
    <itemref idref="ch2"/>
  </spine>
</package>
"""

TOC_NCX = """<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="urn:uuid:crosspoint-render-test-0001"/>
  </head>
  <docTitle><text>CrossPoint Rendering Test</text></docTitle>
  <navMap>
    <navPoint id="np1" playOrder="1">
      <navLabel><text>1. Text Formatting</text></navLabel>
      <content src="ch1.xhtml"/>
    </navPoint>
    <navPoint id="np2" playOrder="2">
      <navLabel><text>2. Image &amp; Code</text></navLabel>
      <content src="ch2.xhtml"/>
    </navPoint>
  </navMap>
</ncx>
"""

NAV_XHTML = """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Contents</title></head>
<body>
  <nav epub:type="toc" id="toc">
    <h1>Contents</h1>
    <ol>
      <li><a href="ch1.xhtml">1. Text Formatting</a></li>
      <li><a href="ch2.xhtml">2. Image &amp; Code</a></li>
    </ol>
  </nav>
</body>
</html>
"""

CH1 = """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Text Formatting</title></head>
<body>
  <h1>Chapter 1: Text Formatting</h1>

  <p>This first paragraph is plain regular text. It establishes a baseline so
  you can compare every styled run below against an unstyled line. The quick
  brown fox jumps over the lazy dog, then does it again to fill the line.</p>

  <p>Here we test <b>bold text using the b tag</b> and
  <strong>bold text using the strong tag</strong>. Next is
  <i>italic text using the i tag</i> and <em>italic text using the em
  tag</em>. Now the combined case:
  <b><i>bold italic via nested b and i</i></b> and
  <strong><em>bold italic via nested strong and em</em></strong>.</p>

  <p>Inline code looks like this: <code>const int RAM = 380;</code> appearing
  mid-sentence, followed by more regular prose so wrapping around the code run
  can be checked carefully on a real page boundary.</p>

  <p>This sentence contains a footnote marker right here<a href="#fn1"
  epub:type="noteref">1</a> and a second one a little further along<a
  href="#fn2" epub:type="noteref">2</a> so footnote collection and the page
  footer rendering can both be exercised.</p>

  <p>A longer mixed paragraph: reading on the <b>Xteink X4</b> means working
  within a <i>tight</i> RAM budget, where <b><i>every allocation matters</i></b>
  and the renderer must stay <em>fast</em> while still handling
  <code>std::string_view</code> safely across the whole pipeline.</p>

  <hr/>
  <aside id="fn1" epub:type="footnote">
    <p>1. This is the body of the first footnote. It should appear at the
    bottom of the page where its marker lands, or in a notes section.</p>
  </aside>
  <aside id="fn2" epub:type="footnote">
    <p>2. The second footnote body, slightly longer, to verify that multiple
    footnotes on a single page wrap and stack correctly.</p>
  </aside>
</body>
</html>
"""

CH2 = """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Image and Code</title></head>
<body>
  <h1>Chapter 2: Image &amp; Code Block</h1>

  <p>The image below is a 300x200 PNG with a grayscale gradient and alternating
  black/white bars, useful for checking image scaling and dithering on e-ink.</p>

  <p><img src="images/test.png" alt="Grayscale gradient and bar test image"/></p>

  <p>After the image, regular text resumes so you can confirm layout flows
  correctly around a block-level image.</p>

  <h2>Code block</h2>
  <p>A preformatted code block follows:</p>
  <pre><code>void onEnter() {
  Activity::onEnter();
  if (!buffer) {
    LOG_ERR("MOD", "malloc failed");
    return;
  }
  render();
}</code></pre>

  <p>And one final line of plain text to mark the end of the test document.</p>
</body>
</html>
"""


def main():
    img_path = os.path.join(ROOT, "_test_img.png")
    make_image(img_path)
    with open(img_path, "rb") as f:
        img_bytes = f.read()
    os.remove(img_path)

    with zipfile.ZipFile(OUT, "w") as z:
        # mimetype MUST be first and stored (uncompressed) per EPUB spec
        zi = zipfile.ZipInfo("mimetype")
        zi.compress_type = zipfile.ZIP_STORED
        z.writestr(zi, "application/epub+zip")

        def add(name, data):
            z.writestr(name, data, compress_type=zipfile.ZIP_DEFLATED)

        add("META-INF/container.xml", CONTAINER_XML)
        add("OEBPS/content.opf", CONTENT_OPF)
        add("OEBPS/toc.ncx", TOC_NCX)
        add("OEBPS/nav.xhtml", NAV_XHTML)
        add("OEBPS/ch1.xhtml", CH1)
        add("OEBPS/ch2.xhtml", CH2)
        add("OEBPS/images/test.png", img_bytes)

    print(f"Wrote {OUT} ({os.path.getsize(OUT)} bytes)")


if __name__ == "__main__":
    main()
