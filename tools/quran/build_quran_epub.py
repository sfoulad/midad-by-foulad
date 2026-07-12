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


def transform_html(text: str) -> str:
    # Ayah numbers: "(N)" -> ornate marker. Only bare digit groups -- the
    # basmala's parenthesised Arabic text is handled separately below.
    text = re.sub(r"\((\d+)\)", lambda m: AYAH_MARKER.format(arabic_digits(m.group(1))), text)
    # Basmala: strip the plain parentheses around it.
    text = text.replace("(بسم الله الرحمن الرحيم)", "بسم الله الرحمن الرحيم")
    # Surah headers: "13- سورة" -> "١٣ - سورة" (Arabic-Indic, spaced dash).
    text = re.sub(r"(\d+)- سورة", lambda m: arabic_digits(m.group(1)) + " - سورة", text)
    return text


def transform_ncx(text: str) -> str:
    return re.sub(r"<text>(\d+)- سورة", lambda m: "<text>" + arabic_digits(m.group(1)) + " - سورة", text)


def transform_opf(text: str) -> str:
    text = re.sub(r"<dc:title>[^<]*</dc:title>", "<dc:title>القرآن الكريم</dc:title>", text)
    # Drop the creator line entirely; the Quran carries no author byline.
    text = re.sub(r"\s*<dc:creator[^>]*>[^<]*</dc:creator>", "", text)
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
            p.write_text(transform_html(p.read_text(encoding="utf-8")), encoding="utf-8")
        elif name == "toc.ncx":
            p.write_text(transform_ncx(p.read_text(encoding="utf-8")), encoding="utf-8")
        elif name == "content.opf":
            p.write_text(transform_opf(p.read_text(encoding="utf-8")), encoding="utf-8")
        elif name == "titlepage.xhtml":
            p.write_text(transform_html(p.read_text(encoding="utf-8")), encoding="utf-8")

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
