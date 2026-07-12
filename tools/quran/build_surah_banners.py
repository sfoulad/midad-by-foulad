#!/usr/bin/env python3
"""Render the 114 ornamental surah-header banners for the firmware Quran.

The template is built from the user-supplied mushaf banner photo
(source/surah_banner.png): the left ornamental half is mirrored to produce a
symmetric band with an empty central cartouche, thresholded to 1-bit for
e-ink. Each surah's name and Arabic-Indic number are shaped with HarfBuzz and
rasterized with FreeType using the bundled Amiri face (the Quran's reading
font), auto-shrunk to fit the cartouche.

Output: build/banners/surah_NNN.png (1-bit, 940x172), consumed by
build_quran_epub.py which swaps each surah <h1> for the matching image.

Requires: pillow, uharfbuzz, freetype-py (same toolchain as the font scripts).
"""

import sys
from pathlib import Path

import freetype
import uharfbuzz as hb
from PIL import Image, ImageOps

HERE = Path(__file__).parent
PHOTO = HERE / "source" / "surah_banner.png"
FONT = HERE / ".." / ".." / "lib" / "EpdFont" / "builtinFonts" / "source" / "Amiri" / "Amiri-Regular.ttf"
OUT_DIR = HERE / "build-banners"

BANNER_W, BANNER_H = 940, 172  # supersampled compose size (2x)
FINAL_W, FINAL_H = 452, 83     # shipped size ~= device viewport width
CARTOUCHE_MAX_TEXT_W = 235     # inner width available for the title text
TEXT_CENTER_Y = 0.52           # cartouche vertical centre (fraction of height)

ARABIC_INDIC = str.maketrans("0123456789", "٠١٢٣٤٥٦٧٨٩")

_hb_font = None
_ft_face = None


def _fonts():
    global _hb_font, _ft_face
    if _hb_font is None:
        _hb_font = hb.Font(hb.Face(hb.Blob.from_file_path(str(FONT))))
        _ft_face = freetype.Face(str(FONT))
    return _hb_font, _ft_face


def render_run(text: str, size_px: int, direction: str) -> Image.Image:
    """Shape one run with HarfBuzz and rasterize it; returns a tight L-mode image
    (black text on white)."""
    font, face = _fonts()
    font.scale = (size_px * 64, size_px * 64)
    face.set_char_size(size_px * 64)
    buf = hb.Buffer()
    buf.add_str(text)
    buf.direction = direction
    buf.script = "Arab"
    buf.language = "ar"
    hb.shape(font, buf)
    glyphs = list(zip(buf.glyph_infos, buf.glyph_positions))
    width = sum(p.x_advance for _, p in glyphs) // 64
    img = Image.new("L", (max(width, 1) + 40, size_px * 3), 255)
    x = 20 * 64
    baseline = int(size_px * 1.6) * 64
    for info, pos in glyphs:
        face.load_glyph(info.codepoint, freetype.FT_LOAD_RENDER)
        bm = face.glyph.bitmap
        gx = (x + pos.x_offset) // 64 + face.glyph.bitmap_left
        gy = baseline // 64 - face.glyph.bitmap_top - pos.y_offset // 64
        if bm.width and bm.rows:
            g = Image.frombytes("L", (bm.width, bm.rows), bytes(bm.buffer))
            img.paste(0, (gx, gy), g)
        x += pos.x_advance
    bbox = img.getbbox()
    return img.crop(bbox) if bbox else img


def build_template() -> Image.Image:
    im = Image.open(PHOTO).convert("L")
    w, h = im.size
    half = im.crop((0, 0, int(w * 0.385), h))
    canvas = Image.new("L", (half.width * 2, h), 255)
    canvas.paste(half, (0, 0))
    canvas.paste(ImageOps.mirror(half), (half.width, 0))
    canvas = canvas.resize((BANNER_W, BANNER_H), Image.LANCZOS)
    tmpl = canvas.point(lambda p: 0 if p < 200 else 255, "L")
    # Clear leftover calligraphy fragments inside the cartouche.
    # Clear only the narrow centre strip where clipped calligraphy fragments
    # from the source photo can survive the mirror seam -- the cartouche's own
    # pointed-oval outline stays intact.
    tmpl.paste(255, (int(BANNER_W * 0.44), int(BANNER_H * 0.18), int(BANNER_W * 0.56), int(BANNER_H * 0.80)))
    return tmpl


def compose_banner(tmpl: Image.Image, title: str, number: int) -> Image.Image:
    digits = str(number).translate(ARABIC_INDIC)
    for size in range(40, 19, -2):  # auto-shrink until the line fits the cartouche
        name = render_run(title, size, "rtl")
        num = render_run(digits, int(size * 0.72), "ltr")
        gap = max(10, size // 3)
        total = name.width + gap + num.width
        if total <= CARTOUCHE_MAX_TEXT_W:
            break
    out = tmpl.copy()
    x0 = (BANNER_W - total) // 2
    cy = int(BANNER_H * TEXT_CENTER_Y)
    # Visual RTL: the name sits right of its number.
    out.paste(num, (x0, cy - num.height // 2), ImageOps.invert(num))
    out.paste(name, (x0 + num.width + gap, cy - name.height // 2), ImageOps.invert(name))
    # Supersampled composition, then downscale to the device's near-native
    # width BEFORE thresholding: the reader draws the image ~1:1 (portrait
    # viewport ~440-450px), so no on-device rescale fuzzes the ornament lines.
    out = out.resize((FINAL_W, FINAL_H), Image.LANCZOS)
    return out.point(lambda p: 0 if p < 176 else 255, "1")


def main() -> int:
    # Surah names from the transformed EPUB's ncx (single source of truth).
    import re
    import zipfile

    epub = HERE / ".." / ".." / "data" / "quran.epub"
    src = HERE / "source" / "Quran.epub"
    with zipfile.ZipFile(epub if epub.exists() else src) as z:
        ncx = z.read("toc.ncx").decode()
    names = re.findall(r"<text>[٠-٩\d]+ ?- (سورة [^<]+)</text>", ncx)
    if len(names) != 114:
        print(f"expected 114 surah names in ncx, found {len(names)}", file=sys.stderr)
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    tmpl = build_template()
    for i, name in enumerate(names, start=1):
        banner = compose_banner(tmpl, name.strip(), i)
        banner.save(OUT_DIR / f"surah_{i:03d}.png", optimize=True)
    total = sum(f.stat().st_size for f in OUT_DIR.glob("surah_*.png"))
    print(f"wrote 114 banners, {total // 1024} KB total")
    return 0


if __name__ == "__main__":
    sys.exit(main())
