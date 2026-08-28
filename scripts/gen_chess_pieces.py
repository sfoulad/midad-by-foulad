#!/usr/bin/env python3
"""Generate the chess piece glyphs for the Chess activity.

Two bitmaps per piece, which is what makes a piece readable on a 1-bit panel
where the dark squares are a dither pattern:

    SILHOUETTE  the whole shape, filled
    INK         the outline plus the interior detail lines

ChessBoardView draws a white piece as silhouette-in-white then ink-in-black,
and a black piece as silhouette-in-black then ink-in-white. Either way the
piece carries its own contrast and never merges into the square under it.

The outline is derived from the silhouette by erosion rather than stroked by
hand, so the two bitmaps can never disagree about where the edge is.

Shapes follow the rounded icon set approved on the design canvas
(design/chess/Pieces.dc.html, set D) and are authored in the same 40-unit space
as those SVG paths. Pillow primitives rather than the SVG source itself, for
the reason gen_app_icons.py already gives: no cairosvg dependency.

Output matches GfxRenderer::drawIcon: square, 1bpp, MSB-first, bit == 0 is ink,
pre-rotated as bitmap[row][col] = screen(size - 1 - row, col).

    python scripts/gen_chess_pieces.py [--preview sheet.png]
"""

import sys

from PIL import Image, ImageDraw, ImageFilter

# Two sizes: the board glyph, and a small one for the captured-piece strips in
# the player bars, where a 42px glyph would not fit the row. Outline weight in
# final pixels -- 2 is the floor at 42px (1px breaks up under the dark squares'
# dither), and the small glyph can only afford 1.
SIZES = [(42, 2), (20, 1)]
SS = 8
OUT_PATH = "src/components/icons/chessPieces.h"

BASE_TOP = 29.0  # y where every piece's body meets its base

# Set per size by main(); the drawing helpers read them.
SIZE = 42
CANVAS = SIZE * SS
U = CANVAS / 40.0  # 40-unit design space -> supersampled pixels
OUTLINE_PX = 2
DETAIL_W = OUTLINE_PX * 40.0 / SIZE


def set_size(size, outline_px):
    global SIZE, CANVAS, U, OUTLINE_PX, DETAIL_W
    SIZE, OUTLINE_PX = size, outline_px
    CANVAS = SIZE * SS
    U = CANVAS / 40.0
    DETAIL_W = OUTLINE_PX * 40.0 / SIZE


def s(v):
    return v * U


def poly(d, points):
    d.polygon([(s(x), s(y)) for x, y in points], fill=255)


def disc(d, cx, cy, r):
    d.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], fill=255)


def box(d, x1, y1, x2, y2, radius=0):
    if radius:
        d.rounded_rectangle([s(x1), s(y1), s(x2), s(y2)], radius=s(radius), fill=255)
    else:
        d.rectangle([s(x1), s(y1), s(x2), s(y2)], fill=255)


def stroke(d, points, width=2.0):
    d.line([(s(x), s(y)) for x, y in points], fill=255, width=int(width * U), joint="curve")


def base(d):
    """The pedestal every piece stands on."""
    box(d, 10, BASE_TOP, 30, 36, radius=2)


# --- silhouettes ------------------------------------------------------------


def pawn(d):
    disc(d, 20, 13.5, 4.0)
    poly(d, [(17.5, 16.5), (22.5, 16.5), (23.5, 20), (16.5, 20)])
    poly(d, [(16.2, 19.5), (23.8, 19.5), (26.5, BASE_TOP), (13.5, BASE_TOP)])
    base(d)


def rook(d):
    poly(d, [(11, 6), (15, 6), (15, 10), (18, 10), (18, 6), (22, 6), (22, 10),
             (25, 10), (25, 6), (29, 6), (29, 14), (11, 14)])
    poly(d, [(13.5, 14), (26.5, 14), (25.2, 20), (25.2, 24), (27.5, BASE_TOP),
             (12.5, BASE_TOP), (14.8, 24), (14.8, 20)])
    base(d)


def knight(d):
    # Head facing left: muzzle at the lower left, ear top right, arched neck.
    poly(d, [(12.5, BASE_TOP), (12.0, 25.5), (13.5, 21.5), (16.5, 18.0),
             (12.5, 19.0), (9.5, 20.5), (7.5, 18.5), (8.5, 15.5), (11.5, 11.5),
             (14.5, 8.5), (16.5, 6.5), (17.5, 3.5), (20.0, 6.5), (24.0, 8.5),
             (27.0, 12.5), (28.0, 18.0), (28.0, BASE_TOP)])
    base(d)


def bishop(d):
    disc(d, 20, 4.5, 2.5)
    poly(d, [(20, 7.5), (24, 12), (27, 18), (27, 23.5), (25.5, BASE_TOP),
             (14.5, BASE_TOP), (13, 23.5), (13, 18), (16, 12)])
    base(d)


def queen(d):
    poly(d, [(12, BASE_TOP), (9.0, 13.5), (13.2, 18.5), (14.0, 9.5), (18.0, 17.0),
             (20.0, 6.0), (22.0, 17.0), (26.0, 9.5), (26.8, 18.5), (31.0, 13.5),
             (28, BASE_TOP)])
    base(d)


def king(d):
    box(d, 18, 2, 22, 13)
    box(d, 14.5, 5.5, 25.5, 9.5)
    poly(d, [(13.5, 14), (26.5, 14), (29.5, 19), (28, BASE_TOP), (12, BASE_TOP),
             (10.5, 19)])
    base(d)


# --- interior detail (drawn on top of the derived outline) -------------------


def detail_common(d):
    # Separator between body and base -- the line the design calls for on
    # every piece, so the pedestal reads as its own block.
    stroke(d, [(12.5, BASE_TOP), (27.5, BASE_TOP)], DETAIL_W)


def detail_pawn(d):
    detail_common(d)


def detail_rook(d):
    detail_common(d)
    stroke(d, [(13.5, 14), (26.5, 14)], DETAIL_W)


def detail_knight(d):
    detail_common(d)
    disc(d, 15.5, 12.0, 1.3)  # eye


def detail_bishop(d):
    detail_common(d)
    stroke(d, [(22.5, 12), (19, 22)], DETAIL_W)  # mitre slit


def detail_queen(d):
    detail_common(d)
    stroke(d, [(12.5, 25), (27.5, 25)], DETAIL_W)  # collar


def detail_king(d):
    detail_common(d)
    stroke(d, [(15.5, 19.5), (17.5, 23)], DETAIL_W)
    stroke(d, [(24.5, 19.5), (22.5, 23)], DETAIL_W)


PIECES = [
    ("Pawn", pawn, detail_pawn),
    ("Knight", knight, detail_knight),
    ("Bishop", bishop, detail_bishop),
    ("Rook", rook, detail_rook),
    ("Queen", queen, detail_queen),
    ("King", king, detail_king),
]


def rasterize(fn):
    """Draw at SS x, downsample, threshold. Returns a mask: 255 = shape."""
    img = Image.new("L", (CANVAS, CANVAS), 0)
    fn(ImageDraw.Draw(img))
    # BOX averages the supersampled block; LANCZOS rings and frays the threshold.
    small = img.resize((SIZE, SIZE), Image.BOX)
    return small.point(lambda p: 255 if p >= 96 else 0, mode="L")


def build(shape_fn, detail_fn):
    silhouette = rasterize(shape_fn)
    # MaxFilter(2k+1) erodes a 255-on-0 mask when applied as MinFilter; the
    # difference between the shape and its erosion is a k-pixel inner outline.
    interior = silhouette.filter(ImageFilter.MinFilter(OUTLINE_PX * 2 + 1))
    detail = rasterize(detail_fn)

    sp, ip, dp = silhouette.load(), interior.load(), detail.load()
    ink = Image.new("L", (SIZE, SIZE), 0)
    kp = ink.load()
    for y in range(SIZE):
        for x in range(SIZE):
            outline = sp[x, y] == 255 and ip[x, y] == 0
            inner_detail = dp[x, y] == 255 and sp[x, y] == 255
            kp[x, y] = 255 if (outline or inner_detail) else 0
    return silhouette, ink


def to_c_array(mask, name):
    """Pack a 255 = shape mask into drawIcon's format (bit == 0 is ink)."""
    px = mask.load()
    row_bytes = (SIZE + 7) // 8
    out = []
    for row in range(SIZE):
        for byte_i in range(row_bytes):
            byte = 0
            for bit in range(8):
                col = byte_i * 8 + bit
                # drawIcon plots bitmap[row][col] at screen(size-1-row, col).
                ink = col < SIZE and px[SIZE - 1 - row, col] == 255
                if not ink:
                    byte |= 1 << (7 - bit)
            out.append(byte)

    body = ""
    # 19 bytes per line fills .clang-format's 120-column limit exactly; see
    # gen_app_icons.py for why a rounder number gets repacked by CI.
    for i in range(0, len(out), 19):
        body += "    " + ", ".join("0x%02X" % b for b in out[i:i + 19]) + ",\n"
    return "static const uint8_t %s[] = {\n%s};\n" % (name, body.rstrip(",\n"))


def main():
    preview = None
    if "--preview" in sys.argv:
        preview = sys.argv[sys.argv.index("--preview") + 1]

    parts = [
        "#pragma once\n#include <cstdint>\n\n",
        "// Chess piece glyphs -- generated by scripts/gen_chess_pieces.py, do not edit by hand.\n",
        "// 1bpp, MSB-first, bit == 0 is ink, pre-rotated for GfxRenderer::drawIcon.\n",
        "// Each piece has a filled SILHOUETTE and an INK layer (outline + detail):\n",
        "// white piece = silhouette in white then ink in black, black piece = the inverse.\n",
    ]
    biggest = max(size for size, _ in SIZES)
    sheet = Image.new("L", (biggest * len(PIECES) * 2, biggest * len(SIZES)), 255)
    for row, (size, outline_px) in enumerate(SIZES):
        set_size(size, outline_px)
        parts.append("\n// --- %dx%d ---\n" % (size, size))
        for i, (name, shape_fn, detail_fn) in enumerate(PIECES):
            silhouette, ink = build(shape_fn, detail_fn)
            parts.append(to_c_array(silhouette, "Chess%sSilhouette%d" % (name, size)))
            parts.append("\n")
            parts.append(to_c_array(ink, "Chess%sInk%d" % (name, size)))
            parts.append("\n")
            sheet.paste(Image.eval(silhouette, lambda p: 255 - p), (i * 2 * biggest, row * biggest))
            sheet.paste(Image.eval(ink, lambda p: 255 - p), ((i * 2 + 1) * biggest, row * biggest))

    with open(OUT_PATH, "w", newline="\n") as f:
        f.write("".join(parts).rstrip("\n") + "\n")
    print("wrote %s (%d pieces x %d sizes)" % (OUT_PATH, len(PIECES), len(SIZES)))

    if preview:
        sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(preview)
        print("preview -> %s" % preview)


if __name__ == "__main__":
    main()
