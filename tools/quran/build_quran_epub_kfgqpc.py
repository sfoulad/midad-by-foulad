#!/usr/bin/env python3
"""Build the firmware-embedded Quran EPUB (data/quran.epub) from the KFGQPC
Uthmanic Hafs source (source/kfgqpc_chapters/chapter-N.xhtml, N=1..114 --
extracted from zeeyado/quran-ebook's "Hafs" EPUB, Quran.com API text, Madinah
Mushaf orthography). Supersedes build_quran_epub.py's Calibre-sourced,
simplified-orthography input.

Transformations applied per surah:
  1. Ayah-number <span class="ayah-number">digits</span> markers become our
     ornate-parenthesis convention with Arabic-Indic digits (already
     Arabic-Indic in the source): "﴿١٢٣﴾". U+FD3E/U+FD3F, same as
     build_quran_epub.py -- GfxRenderer::drawArabicText's ayah-marker branch
     is unchanged, so this needs no firmware code changes.
  2. WORD JOINER (U+2060) / ZERO WIDTH SPACE (U+200B) formatting characters
     around each ayah number (there for KOReader/CREngine's own line-breaking,
     which our renderer doesn't need) are dropped during extraction.
  3. Bismillah: the source omits it entirely for Surah 9 (no
     bismillah-table -- correct per tradition) and folds it into ayah 1's own
     text for Surah 1 (Al-Fatiha, where it IS the first ayah). Every other
     surah has a separate bismillah-table with no vocalization; we detect its
     presence and prepend the real ligature glyph, U+FDFD, wrapped in our
     U+E005/U+E006 marker convention -- GfxRenderer::drawArabicText's Bismillah
     branch renders it from a tiny dedicated font (QuranCommon) bundled just
     for this glyph, since UthmanicHafs itself lacks it (see main.cpp's font
     registration comment).
  4. Surah headers: a single-row banner styled after the Madinah Mushaf's own
     surah-heading design -- rule lines above/below, ayah-count label (right),
     the surah's calligraphic name (center, baked from the source's own
     surah-name-v4.ttf via fontconvert.py --glyph-map since that font has no
     cmap entries at all), revelation-order label (left). Wrapped in
     U+E007-E009 sentinels -- see surah_banner_marker() and
     GfxRenderer::parseSurahBannerMarker. Ayah count is counted directly from
     each chapter's ayah-number spans; revelation order comes from the
     REVELATION_ORDER table below (the source has no such data).
  5. Metadata, cover, and the mimetype/container.xml/stylesheet.css
     boilerplate are otherwise the same as build_quran_epub.py's output --
     none of that depends on the source text at all.

Output is a deterministic zip (fixed timestamps) so rebuilding without
source changes produces a byte-identical asset.

Usage: python3 build_quran_epub_kfgqpc.py   (from tools/quran/)
"""

import re
import shutil
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

HERE = Path(__file__).parent
SOURCE_DIR = HERE / "source" / "kfgqpc_chapters"
COVER = HERE / "source" / "quran_cover.png"
OUT = HERE / ".." / ".." / "data" / "quran.epub"
WORK = HERE / "build_kfgqpc"

SURAH_COUNT = 114

XHTML_NS = "http://www.w3.org/1999/xhtml"
EPUB_NS = "http://www.idpf.org/2007/ops"
NS = {"x": XHTML_NS, "epub": EPUB_NS}

ARABIC_INDIC = str.maketrans("0123456789", "٠١٢٣٤٥٦٧٨٩")
# Logical order: ORNATE RIGHT PAREN (U+FD3F, the opening one in RTL text),
# digits, ORNATE LEFT PAREN (U+FD3E). Same convention as build_quran_epub.py --
# GfxRenderer::parseAyahMarker recognizes this exact byte sequence.
AYAH_MARKER = "﴿{}﴾"

BASMALA_VOCALIZED = "بِسْمِ اللَّهِ الرَّحْمَنِ الرَّحِيمِ"  # kept as a documented fallback; unused below

# Logical order: U+E005, the real Bismillah ligature (U+FDFD), U+E006. Same
# build-tool-controlled PUA-sentinel convention as AYAH_MARKER above --
# GfxRenderer::parseBismillahMarker recognizes this exact byte sequence. A
# space on each side for the same reason AYAH_MARKER needs one (see below):
# the marker must be its own isolated token, or GfxRenderer's exact-match
# parse never fires and the whole thing falls through to the plain-text path,
# where U+FDFD has no glyph in any general-purpose reading font and silently
# vanishes.
BISMILLAH_MARKER = " ﷽ "

# Standard chronological (nuzul) revelation order, index = revelation order
# (1-based), value = Mushaf surah number -- the widely-published Al-Azhar
# committee chronology (matches Wikipedia's "Chronological order of the
# Quran" and corpus.quran.com). Al-Alaq (96) is universally agreed as the
# first revelation; Al-Ma'idah (5) as the last. Verified programmatically to
# contain each of 1..114 exactly once before use.
REVELATION_ORDER = [
    96, 68, 73, 74, 1, 111, 81, 87, 92, 89,
    93, 94, 103, 100, 108, 102, 107, 109, 105, 113,
    114, 112, 53, 80, 97, 91, 85, 95, 106, 101,
    75, 104, 77, 50, 90, 86, 54, 38, 7, 72,
    36, 25, 35, 19, 20, 56, 26, 27, 28, 17,
    10, 11, 12, 15, 6, 37, 31, 34, 39, 40,
    41, 42, 43, 44, 45, 46, 51, 88, 18, 16,
    71, 14, 21, 23, 32, 52, 67, 69, 70, 78,
    79, 82, 84, 30, 29, 83, 2, 8, 3, 33,
    60, 4, 99, 57, 47, 13, 55, 76, 65, 98,
    59, 110, 24, 22, 63, 58, 49, 66, 64, 61,
    62, 48, 9, 5,
]
assert len(REVELATION_ORDER) == SURAH_COUNT and set(REVELATION_ORDER) == set(range(1, SURAH_COUNT + 1))
REVELATION_ORDER_FOR_SURAH = {surah: order for order, surah in enumerate(REVELATION_ORDER, start=1)}

# Must match GfxRenderer.cpp's SURAH_BANNER_NAME_BASE_CP -- surah n's calligraphic
# name glyph (baked via fontconvert.py --glyph-map, see
# gen_surah_banner_glyphmap.py) lives at this codepoint plus (n - 1).
SURAH_BANNER_NAME_BASE_CP = 0xE010


def surah_banner_marker(surah: int, ayah_count: int, revelation_order: int) -> str:
    """GfxRenderer::parseSurahBannerMarker's wire format: U+E007, the surah's
    name-glyph codepoint (literal char), the ayah-count label, U+E009, the
    revelation-order label, U+E008. See that function's comment for why the
    labels are plain composed Arabic text rather than re-derived in C++.
    The space inside each label uses CARTOUCHE_SPACE_CP (U+E004), not a real
    space character -- ChapterHtmlSlimParser tokenizes on whitespace bytes
    before the marker ever reaches GfxRenderer, so a literal space here would
    fragment the marker across multiple draw calls and parseSurahBannerMarker
    would never see it whole (confirmed via simulator: labels rendered as
    plain text with no calligraphy or rule lines -- exactly what plain-text
    fallback rendering of the fragments looks like). Same fix the cartouche
    marker already needed for surah names containing spaces.
    """
    name_glyph = chr(SURAH_BANNER_NAME_BASE_CP + (surah - 1))
    label_space = ""
    right_label = "آياتها" + label_space + arabic_digits(str(ayah_count))
    left_label = "ترتيبها" + label_space + arabic_digits(str(revelation_order))
    return "" + name_glyph + right_label + "" + left_label + ""


# WORD JOINER / ZERO WIDTH SPACE: formatting characters the source EPUB relies
# on for KOReader/CREngine's own line-breaking around ayah numbers. Our
# ChapterHtmlSlimParser does its own word-breaking and doesn't need them --
# left in, they fuse the surrounding words into one unbreakable token (see
# plan notes: this is exactly why the source EPUB can't be fed to our reader
# directly).
STRIP_CHARS = "⁠​"


def arabic_digits(n: str) -> str:
    return n.translate(ARABIC_INDIC)


def local_tag(elem: ET.Element) -> str:
    return elem.tag.rsplit("}", 1)[-1]


def extract_surah_text(div: ET.Element) -> str:
    """Concatenate a <div class="surah-text">'s content in document order,
    rewriting <span class="ayah-number"> into our marker convention and
    dropping pagebreak spans (and the WJ/ZWS characters around ayah numbers)
    entirely. Any other inline element (e.g. a hizb-marker span) is treated
    as plain text -- its .text/.tail still gets included normally."""
    parts = []
    if div.text:
        parts.append(div.text)
    for child in div:
        if local_tag(child) == "span" and child.get("class") == "ayah-number":
            digits = (child.text or "").strip()
            # A space on each side: GfxRenderer::parseAyahMarker only recognizes the
            # marker when it's the ENTIRE text of a draw call, and the reading
            # engine's word-breaking splits purely on whitespace bytes -- without
            # this, the marker fuses onto whichever word sits next to it in the
            # source (which has no space here, unlike build_quran_epub.py's source),
            # parseAyahMarker's exact-match never fires, and the whole marker falls
            # through to the plain-text path, where U+FD3E/FD3F have no font glyph
            # and silently vanish -- leaving just a bare digit with no ayah rosette
            # (confirmed via simulator: every marker logged matched=0).
            parts.append(" " + AYAH_MARKER.format(digits) + " ")
        elif child.text:
            parts.append(child.text)
        if child.tail:
            parts.append(child.tail)
    text = "".join(parts)
    for ch in STRIP_CHARS:
        text = text.replace(ch, "")
    return text


def build_surah_html(surah: int, name: str, ayah_text: str, ayah_count: int, bismillah: str = "") -> str:
    # Header: a single row styled after the Madinah Mushaf's own surah-heading
    # banner -- rule lines above/below, ayah-count label (right), the surah's
    # calligraphic name (center, baked from surah-name-v4.ttf), revelation-order
    # label (left). Replaces the old code-drawn medallion+cartouche pair. <h1>
    # (not <p>) so ChapterHtmlSlimParser's HEADER_TAGS branch lets .block_2's own
    # CSS text-align win over the reader's paragraph-alignment setting -- same
    # reasoning as ParsedText's Bismillah-line override, just via the existing
    # header exemption instead of a new special case.
    revelation_order = REVELATION_ORDER_FOR_SURAH[surah]
    banner = surah_banner_marker(surah, ayah_count, revelation_order)
    header = f'<h1 class="block_2" dir="rtl" id="toc_id_{surah}">{banner}</h1>'
    # The Bismillah gets its OWN centered paragraph (.block_ is text-align:center
    # by default), matching the source's own separate <table class="bismillah-table">
    # -- NOT concatenated into the ayah_text paragraph below, where it would flow
    # inline as the first "word" of the first ayah's line instead of standing alone
    # above it, the way every printed Mushaf sets it.
    bismillah_line = f'<p class="block_" dir="rtl">{bismillah}</p>' if bismillah else '<p class="block_" dir="rtl"> </p>'
    body = f'{bismillah_line}<p class="block_" dir="rtl">{ayah_text}</p><p class="block_1" dir="rtl"> </p>'
    return (
        "<?xml version='1.0' encoding='utf-8'?>\n"
        '<html xmlns="http://www.w3.org/1999/xhtml" lang="ar" xml:lang="ar">\n'
        "  <head>\n"
        f"    <title>{name}</title>\n"
        '    <meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>\n'
        '  <link rel="stylesheet" type="text/css" href="../stylesheet.css"/>\n'
        "</head>\n"
        '  <body class="calibre">\n'
        f"{header}{body}\n"
        "\t</body>\n"
        "</html>"
    )


STYLESHEET_CSS = """.block_ {
  direction: rtl;
  display: block;
  font-weight: bold;
  text-align: center;
  margin: 0;
  padding: 0;
}
.block_1 {
  direction: rtl;
  display: block;
  font-family: serif;
  font-size: 0.75em;
  font-weight: bold;
  text-align: center;
  margin: 0;
  padding: 0;
}
.block_2 {
  display: block;
  font-size: 2em;
  font-weight: bold;
  line-height: 1.2;
  page-break-after: avoid;
  text-align: center;
  margin: 6pt 0 3pt -0.1pt;
  padding: 1pt;
  border: black double 3pt;
}
.calibre {
  color: black;
  display: block;
  font-family: "Times New Roman", serif;
  font-size: 1em;
  padding-left: 0;
  padding-right: 0;
  margin: 0 5pt;
}
"""

CONTAINER_XML = """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
   <rootfiles>
      <rootfile full-path="content.opf" media-type="application/oebps-package+xml"/>
   </rootfiles>
</container>
"""

TITLEPAGE_XHTML = """<?xml version='1.0' encoding='utf-8'?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">
    <head>
        <meta http-equiv="Content-Type" content="text/html; charset=UTF-8"/>
        <title>Cover</title>
        <style type="text/css" title="override_css">
            @page {padding: 0pt; margin:0pt}
            body { text-align: center; padding:0pt; margin: 0pt; }
        </style>
    </head>
    <body>
        <div>
            <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" version="1.1" width="100%" height="100%" viewBox="0 0 600 880" preserveAspectRatio="none">
                <image width="600" height="880" xlink:href="images/cover.png"/>
            </svg>
        </div>
    </body>
</html>
"""


def build_content_opf(surah_names: list[str]) -> str:
    manifest_items = [
        '    <item id="cover" href="images/cover.png" media-type="image/png"/>',
        '    <item id="titlepage" href="titlepage.xhtml" media-type="application/xhtml+xml"/>',
    ]
    spine_items = ['    <itemref idref="titlepage"/>']
    for n in range(1, SURAH_COUNT + 1):
        manifest_items.append(f'    <item id="surah{n}" href="text/surah-{n:03d}.html" media-type="application/xhtml+xml"/>')
        spine_items.append(f'    <itemref idref="surah{n}"/>')
    manifest_items.append('    <item id="css" href="stylesheet.css" media-type="text/css"/>')
    manifest_items.append('    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>')
    return (
        "<?xml version='1.0' encoding='utf-8'?>\n"
        '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uuid_id">\n'
        '  <metadata xmlns:opf="http://www.idpf.org/2007/opf" xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:language>ar</dc:language>\n"
        "    <dc:title>القرآن الكريم</dc:title>\n"
        '    <dc:identifier id="uuid_id" opf:scheme="uuid">f4c9a3e0-4b8e-4b41-9a2c-2f9c7c0e7e3a</dc:identifier>\n'
        '    <meta name="cover" content="cover"/>\n'
        "  </metadata>\n"
        "  <manifest>\n" + "\n".join(manifest_items) + "\n  </manifest>\n"
        '  <spine toc="ncx" page-progression-direction="rtl">\n' + "\n".join(spine_items) + "\n  </spine>\n"
        "  <guide>\n"
        '    <reference type="cover" href="titlepage.xhtml" title="Cover"/>\n'
        "  </guide>\n"
        "</package>\n"
    )


def build_toc_ncx(surah_names: list[str]) -> str:
    nav_points = []
    for n, name in enumerate(surah_names, start=1):
        # surah_names carries the source's own "سورة <name>" title verbatim (see main()
        # below), but every single one of the 114 entries starts with that same word --
        # zero information, just clutter in the "Select Chapter" list and (before the
        # digit prefix here was stripped in the reader's status bar too) a truncation
        # trap for RTL text. Show the bare name directly, e.g. "44 - الدخان" not
        # "44 - سورة الدخان".
        bare_name = name[len("سورة "):] if name.startswith("سورة ") else name
        label = f"{arabic_digits(str(n))} - {bare_name}"
        nav_points.append(
            f'    <navPoint id="num_{n}" playOrder="{n}" class="chapter">\n'
            f"      <navLabel>\n"
            f"        <text>{label}</text>\n"
            f"      </navLabel>\n"
            f'      <content src="text/surah-{n:03d}.html"/>\n'
            f"    </navPoint>"
        )
    return (
        "<?xml version='1.0' encoding='utf-8'?>\n"
        '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1" xml:lang="ara">\n'
        "  <head>\n"
        '    <meta name="dtb:uid" content="f4c9a3e0-4b8e-4b41-9a2c-2f9c7c0e7e3a"/>\n'
        '    <meta name="dtb:depth" content="1"/>\n'
        "  </head>\n"
        "  <docTitle>\n"
        "    <text>القرآن الكريم</text>\n"
        "  </docTitle>\n"
        "  <navMap>\n" + "\n".join(nav_points) + "\n  </navMap>\n"
        "</ncx>\n"
    )


def main() -> int:
    if not SOURCE_DIR.exists():
        print(f"source not found: {SOURCE_DIR}", file=sys.stderr)
        return 1

    if WORK.exists():
        shutil.rmtree(WORK)
    (WORK / "text").mkdir(parents=True)
    (WORK / "images").mkdir()
    (WORK / "META-INF").mkdir()

    surah_names: list[str] = []
    for n in range(1, SURAH_COUNT + 1):
        src = SOURCE_DIR / f"chapter-{n}.xhtml"
        if not src.exists():
            print(f"missing source chapter: {src}", file=sys.stderr)
            return 1
        raw = src.read_text(encoding="utf-8")
        root = ET.fromstring(raw)
        title_elem = root.find(".//x:title", NS)
        name = (title_elem.text or "").strip() if title_elem is not None else f"سورة {arabic_digits(str(n))}"
        surah_names.append(name)

        div = root.find('.//x:div[@class="surah-text"]', NS)
        if div is None:
            print(f"no surah-text div in chapter-{n}.xhtml", file=sys.stderr)
            return 1
        ayah_text = extract_surah_text(div)
        ayah_count = len(div.findall('.//x:span[@class="ayah-number"]', NS))
        bismillah = BISMILLAH_MARKER if "bismillah-table" in raw else ""

        (WORK / "text" / f"surah-{n:03d}.html").write_text(
            build_surah_html(n, name, ayah_text, ayah_count, bismillah), encoding="utf-8"
        )

    (WORK / "stylesheet.css").write_text(STYLESHEET_CSS, encoding="utf-8")
    (WORK / "titlepage.xhtml").write_text(TITLEPAGE_XHTML, encoding="utf-8")
    (WORK / "content.opf").write_text(build_content_opf(surah_names), encoding="utf-8")
    (WORK / "toc.ncx").write_text(build_toc_ncx(surah_names), encoding="utf-8")
    (WORK / "META-INF" / "container.xml").write_text(CONTAINER_XML, encoding="utf-8")
    (WORK / "images" / "cover.png").write_bytes(COVER.read_bytes())
    (WORK / "mimetype").write_text("application/epub+zip", encoding="utf-8")

    names = ["mimetype", "META-INF/container.xml", "content.opf", "toc.ncx", "titlepage.xhtml", "stylesheet.css",
             "images/cover.png"] + [f"text/surah-{n:03d}.html" for n in range(1, SURAH_COUNT + 1)]

    OUT.parent.mkdir(parents=True, exist_ok=True)
    # EPUB spec: "mimetype" first and stored uncompressed. Fixed date_time for
    # deterministic output.
    with zipfile.ZipFile(OUT, "w") as z:
        for name in names:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            z.writestr(info, (WORK / name).read_bytes())

    shutil.rmtree(WORK)
    print(f"wrote {OUT.resolve()} ({OUT.stat().st_size} bytes, {SURAH_COUNT} surahs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
