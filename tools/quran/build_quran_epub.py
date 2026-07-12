#!/usr/bin/env python3
"""Build the firmware-embedded Quran EPUB (data/quran.epub) from source/Quran.epub.

Transformations applied to the source (a plain Calibre-converted Arabic Quran):
  1. Ayah numbers "(N)" become traditional ornate-parenthesis markers with
     Arabic-Indic digits: "﴿١٢٣﴾" (renders as ﴿١٢٣﴾). The built-in
     Noto Naskh/Sans Arabic fonts carry U+FD3E/U+FD3F specifically for this
     (lib/EpdFont/scripts/fontconvert.py arabic interval set).
  2. The basmala line loses its Western parentheses.
  3. Surah headers and the TOC swap Western digits for Arabic-Indic ones
     ("13- سورة الرعد" -> "١٣ - سورة الرعد").
  4. Metadata: title "القرآن الكريم", no author line.

Output is a deterministic zip (fixed timestamps) so rebuilding without source
changes produces a byte-identical asset, keeping the firmware image stable.

Usage: python3 build_quran_epub.py   (from tools/quran/)
"""

import re
import shutil
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).parent
SOURCE = HERE / "source" / "Quran.epub"
OUT = HERE / ".." / ".." / "data" / "quran.epub"
WORK = HERE / "build"

ARABIC_INDIC = str.maketrans("0123456789", "٠١٢٣٤٥٦٧٨٩")
# Logical order: ORNATE RIGHT PAREN (U+FD3F, the opening one in RTL text),
# digits, ORNATE LEFT PAREN (U+FD3E).
AYAH_MARKER = "﴿{}﴾"


def arabic_digits(n: str) -> str:
    return n.translate(ARABIC_INDIC)


# The source writes the basmala unvocalized; the rest of the text carries full
# tashkeel, so the most-recited line of all looked bare (user report).
BASMALA_PLAIN = "بسم الله الرحمن الرحيم"
BASMALA_VOCALIZED = "بِسْمِ اللَّهِ الرَّحْمَنِ الرَّحِيمِ"


def transform_html(text: str, surah: int | None = None) -> str:
    # Ayah numbers: "(N)" -> ornate marker. Only bare digit groups -- the
    # basmala's parenthesised Arabic text is handled separately below.
    text = re.sub(r"\((\d+)\)", lambda m: AYAH_MARKER.format(arabic_digits(m.group(1))), text)
    # Basmala: strip the plain parentheses and vocalize it.
    text = text.replace("(" + BASMALA_PLAIN + ")", BASMALA_PLAIN)
    text = text.replace(BASMALA_PLAIN, BASMALA_VOCALIZED)
    # Surah headers: NOT a raster image (a first attempt shipping 114
    # pre-rendered banner PNGs came out as visual noise on real e-ink hardware
    # -- the reader's <img> layout box doesn't preserve the banner's native
    # pixel grid, and rescaling an already-dithered 1-bit image produces
    # moire). Two text lines instead, each rendering through the exact same
    # font/shaping/layout pipeline as body text, so neither can come out
    # wrong the way an image can:
    #   1. The surah NUMBER as its own right-aligned line: a code-drawn
    #      medallion (GfxRenderer::drawArabicText's surah-medallion branch,
    #      triggered by wrapping the digits in U+E000/U+E001 -- Private Use
    #      Area sentinels this tool fully controls, so they can never appear
    #      in real book text). Deliberately a DIFFERENT shape from the ayah
    #      marker's rosette-glyph medallion (filled disc, inverted digits)
    #      and deliberately NOT sharing a line with the name, per explicit
    #      user direction.
    #   2. The surah NAME, centered, flanked by U+06DE RUB EL HIZB -- an
    #      authentic Quranic manuscript section-marker glyph Amiri renders
    #      natively.
    if surah is not None:
        surah_marker = "\ue000" + arabic_digits(str(surah)) + "\ue001"

        def replace_header(m: "re.Match[str]") -> str:
            toc_id = m.group(1)
            inner = m.group(2)
            name = re.sub(r"^\d+-\s*", "", inner)  # strip the leading "N- " the source writes
            return (
                f'<p class="block_2" dir="rtl" id="{toc_id}" style="text-align: right">{surah_marker}</p>'
                f'<h1 class="block_2" dir="rtl">\u06de {name} \u06de</h1>'
            )

        text = re.sub(
            r'<h1 class="block_2" dir="rtl" id="(toc_id_\d+)">([^<]*)</h1>',
            replace_header,
            text)
    # Remaining headers (title page etc.): Arabic-Indic digits.
    text = re.sub(r"(\d+)- سورة", lambda m: arabic_digits(m.group(1)) + " - سورة", text)
    return text


def transform_ncx(text: str) -> str:
    return re.sub(r"<text>(\d+)- سورة", lambda m: "<text>" + arabic_digits(m.group(1)) + " - سورة", text)


def transform_opf(text: str) -> str:
    text = re.sub(r"<dc:title>[^<]*</dc:title>", "<dc:title>القرآن الكريم</dc:title>", text)
    # Drop the creator line entirely; the Quran carries no author byline.
    text = re.sub(r"\s*<dc:creator[^>]*>[^<]*</dc:creator>", "", text)
    # The ornamental line-art cover ships as a 1-bit PNG (crisper on e-ink and
    # ~6% of the source JPEG's size); repoint the manifest entry.
    text = text.replace('href="images/00001.jpeg" media-type="image/jpeg"',
                        'href="images/cover.png" media-type="image/png"')
    return text


def main() -> int:
    if not SOURCE.exists():
        print(f"source not found: {SOURCE}", file=sys.stderr)
        return 1

    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir()
    with zipfile.ZipFile(SOURCE) as z:
        names = z.namelist()
        z.extractall(WORK)

    for name in names:
        p = WORK / name
        if name.startswith("text/") and name.endswith(".html"):
            raw = p.read_text(encoding="utf-8")
            m = re.search(r'id="toc_id_(\d+)"', raw)
            surah = int(m.group(1)) if m else None
            p.write_text(transform_html(raw, surah), encoding="utf-8")
        elif name == "toc.ncx":
            p.write_text(transform_ncx(p.read_text(encoding="utf-8")), encoding="utf-8")
        elif name == "content.opf":
            p.write_text(transform_opf(p.read_text(encoding="utf-8")), encoding="utf-8")
        elif name == "titlepage.xhtml":
            tp = transform_html(p.read_text(encoding="utf-8"))
            tp = tp.replace('viewBox="0 0 314 475"', 'viewBox="0 0 600 880"')
            tp = tp.replace('<image width="314" height="475" xlink:href="images/00001.jpeg"/>',
                            '<image width="600" height="880" xlink:href="images/cover.png"/>')
            p.write_text(tp, encoding="utf-8")

    # Cover only (no surah banner images -- see the transform_html comment
    # above for why those were dropped): 1-bit ornamental PNG in, source
    # JPEG out.
    (WORK / "images").mkdir(exist_ok=True)
    (WORK / "images" / "cover.png").write_bytes((HERE / "source" / "quran_cover.png").read_bytes())
    (WORK / "images" / "00001.jpeg").unlink(missing_ok=True)
    names = [n for n in names if n != "images/00001.jpeg"] + ["images/cover.png"]

    OUT.parent.mkdir(parents=True, exist_ok=True)
    # EPUB spec: "mimetype" first and stored uncompressed. Fixed date_time for
    # deterministic output.
    with zipfile.ZipFile(OUT, "w") as z:
        ordered = ["mimetype"] + [n for n in names if n != "mimetype"]
        ordered = [n for n in ordered if not (WORK / n).is_dir()]  # skip directory entries
        for name in ordered:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            z.writestr(info, (WORK / name).read_bytes())

    shutil.rmtree(WORK)
    print(f"wrote {OUT.resolve()} ({OUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
