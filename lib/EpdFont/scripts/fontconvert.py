#!python3
import zlib
import unicodedata
import sys
import re
import math
import argparse
from collections import namedtuple

# Force UTF-8 stdout so that `python fontconvert.py … > foo.h` on Windows
# (default cp1252) doesn't emit UTF-16 LE / replacement chars in the generated
# header. Wrapped in a hasattr guard so it's a no-op on older Pythons.
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')

# Originally from https://github.com/vroland/epdiy

parser = argparse.ArgumentParser(description="Generate a header file from a font to be used with epdiy.")
parser.add_argument("name", action="store", help="name of the font.")
parser.add_argument("size", type=int, help="font size to use.")
parser.add_argument("fontstack", action="store", nargs='+', help="list of font files, ordered by descending priority.")
parser.add_argument("--2bit", dest="is2Bit", action="store_true", help="generate 2-bit greyscale bitmap instead of 1-bit black and white.")
parser.add_argument("--additional-intervals", dest="additional_intervals", action="append", help="Additional code point intervals to export as min,max. This argument can be repeated.")
parser.add_argument("--compress", dest="compress", action="store_true", help="Compress glyph bitmaps using DEFLATE with group-based compression.")
parser.add_argument("--force-autohint", dest="force_autohint", action="store_true", help="Force FreeType auto-hinter instead of native font hinting. Improves stem width consistency for fonts with weak or no native TrueType hints.")
parser.add_argument("--pnum", dest="pnum", action="store_true", help="Use proportional numerals (pnum OpenType feature) instead of default tabular figures. Reduces visual gaps between digits in running prose.")
parser.add_argument("--shape-fallback", dest="shape_fallback", action="store_true", help="Synthesize Arabic presentation-form glyphs missing from cmap by shaping the base letters through the font's own OpenType GSUB rules (HarfBuzz). Needed for Quranic fonts (Neirizi, KFGQPC) that only expose contextual forms via GSUB.")
parser.add_argument("--reposition-marks", dest="reposition_marks", action="store_true", help="Move combining marks (harakat) to fixed above/below-baseline heights. For fonts that anchor marks at the baseline and rely on OpenType GPOS mark attachment, which the firmware renderer doesn't run.")
parser.add_argument("--contrast-gamma", dest="contrast_gamma", type=float, default=1.0, help="Gamma-correct each glyph's raw anti-aliased coverage before quantizing: v' = min(255, round(255 * (v/255)**gamma)). gamma<1.0 boosts mid-gray coverage toward black -- for fonts whose fine strokes anti-alias into mostly-gray pixels at small e-ink sizes, which then read as faded once quantized to --2bit's 4 levels. 1.0 (default) is a no-op. Only affects ink darkness, never glyph metrics (advance/bearing), so it can't perturb layout or kashida-point math.")
parser.add_argument("--script", dest="script", choices=["latin", "arabic", "none"], default="latin", help="Base code point interval set to export. 'latin' (default) is the existing Latin/Cyrillic/etc. set used by all bundled reading fonts. 'arabic' swaps in a minimal set (basic Latin punctuation/digits + Arabic blocks) sized for a dedicated Arabic-only font, not the full multi-script coverage. 'none' starts from an empty base set, relying entirely on --additional-intervals -- for narrow single-purpose fonts (e.g. a font bundling just one specific ligature glyph) where even 'arabic''s minimal set would pull in unwanted incidental cmap entries.")
parser.add_argument("--glyph-map", dest="glyph_map", help="Path to a TSV file mapping arbitrary trigger strings to target codepoints, for fonts that only expose glyphs via GSUB contextual substitution triggered by a specific input string (no direct cmap entry, and no Unicode presentation-form decomposition for --shape-fallback to key off). Each line: '<hex-codepoint>\\t<trigger-string>\\t<shaped-glyph-index>' -- the trigger is shaped once via HarfBuzz against the FIRST fontstack face, and the glyph at the given index in the shaped output is baked as that codepoint's glyph (must also appear in --additional-intervals to actually be exported). Same load_glyph() override mechanism as --pnum/--shape-fallback.")
args = parser.parse_args()

import freetype
from fontTools.ttLib import TTFont

GlyphProps = namedtuple("GlyphProps", ["width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"])

font_stack = [freetype.Face(f) for f in args.fontstack]
is2Bit = args.is2Bit
size = args.size
font_name = args.name
load_flags = freetype.FT_LOAD_RENDER
if args.force_autohint:
    load_flags |= freetype.FT_LOAD_FORCE_AUTOHINT

# inclusive unicode code point intervals
# must not overlap and be in ascending order
intervals = [
    ### Basic Latin ###
    # ASCII letters, digits, punctuation, control characters
    (0x0000, 0x007F),
    ### Latin-1 Supplement ###
    # Accented characters for Western European languages
    (0x0080, 0x00FF),
    ### Latin Extended-A ###
    # Eastern European and Baltic languages
    (0x0100, 0x017F),
    ### Latin Extended-B (Vietnamese subset only) ###
    # Only Ơ/ơ (U+01A0-01A1), Ư/ư (U+01AF-01B0) for Vietnamese
    (0x01A0, 0x01A1),
    (0x01AF, 0x01B0),
    ### Latin Extended-B (European subset only) ###
    # Croatian digraphs (DŽ/Lj/Nj), Pinyin caron variants,
    # European diacritical variants, Romanian (Ș/ș/Ț/ț)
    (0x01C4, 0x021F),
    ### Vietnamese Extended ###
    # All precomposed Vietnamese characters with tone marks
    # Ả Ấ Ầ Ẩ Ẫ Ậ Ắ Ằ Ẳ Ẵ Ặ Ẹ Ẻ Ẽ Ế Ề Ể Ễ Ệ Ỉ Ị Ọ Ỏ Ố Ồ Ổ Ỗ Ộ Ớ Ờ Ở Ỡ Ợ Ụ Ủ Ứ Ừ Ử Ữ Ự Ỳ Ỵ Ỷ Ỹ
    (0x1EA0, 0x1EF9),
    ### General Punctuation (core subset) ###
    # Smart quotes, en dash, em dash, ellipsis, NO-BREAK SPACE
    (0x2000, 0x206F),
    ### Basic Symbols From "Latin-1 + Misc" ###
    # dashes, quotes, prime marks
    (0x2010, 0x203A),
    # misc punctuation
    (0x2040, 0x205F),
    # common currency symbols
    (0x20A0, 0x20CF),
    ### Combining Diacritical Marks (minimal subset) ###
    # Needed for proper rendering of many extended Latin languages
    (0x0300, 0x036F),
    ### Greek & Coptic ###
    # Used in science, maths, philosophy, some academic texts
    # (0x0370, 0x03FF),
    ### Cyrillic ###
    # Russian, Ukrainian, Bulgarian, etc.
    (0x0400, 0x04FF),
    ### Math Symbols (common subset) ###
    # Superscripts and Subscripts
    (0x2070, 0x209F),
    # General math operators
    (0x2200, 0x22FF),
    # Arrows
    (0x2190, 0x21FF),
    ### CJK ###
    # Core Unified Ideographs
    # (0x4E00, 0x9FFF),
    # # Extension A
    # (0x3400, 0x4DBF),
    # # Extension B
    # (0x20000, 0x2A6DF),
    # # Extension C–F
    # (0x2A700, 0x2EBEF),
    # # Extension G
    # (0x30000, 0x3134F),
    # # Hiragana
    # (0x3040, 0x309F),
    # # Katakana
    # (0x30A0, 0x30FF),
    # # Katakana Phonetic Extensions
    # (0x31F0, 0x31FF),
    # # Halfwidth Katakana
    # (0xFF60, 0xFF9F),
    # # Hangul Syllables
    # (0xAC00, 0xD7AF),
    # # Hangul Jamo
    # (0x1100, 0x11FF),
    # # Hangul Compatibility Jamo
    # (0x3130, 0x318F),
    # # Hangul Jamo Extended-A
    # (0xA960, 0xA97F),
    # # Hangul Jamo Extended-B
    # (0xD7B0, 0xD7FF),
    # # CJK Radicals Supplement
    # (0x2E80, 0x2EFF),
    # # Kangxi Radicals
    # (0x2F00, 0x2FDF),
    # # CJK Symbols and Punctuation
    # (0x3000, 0x303F),
    # # CJK Compatibility Forms
    # (0xFE30, 0xFE4F),
    # # CJK Compatibility Ideographs
    # (0xF900, 0xFAFF),
    ### Alphabetic Presentation Forms (Latin ligatures) ###
    # ff, fi, fl, ffi, ffl, long-st, st
    (0xFB00, 0xFB06),
    ### Specials
    # Replacement Character
    (0xFFFD, 0xFFFD),
]

if args.script == "arabic":
    # Minimal set for a dedicated Arabic-only font (ArabicFontSystem): digits/space/
    # ASCII punctuation that can appear inline in mixed Arabic-Latin titles, the
    # ellipsis truncatedText() appends, and the three Arabic Unicode blocks
    # ArabicShaper's shaping tables target (matches the SD-card Arabic preset in
    # fontconvert_sdcard.py's INTERVAL_PRESETS["arabic"]). Deliberately excludes the
    # Cyrillic/Greek/CJK ranges above -- this font is never used for non-Arabic text.
    intervals = [
        (0x0020, 0x007E),  # Basic Latin: digits, space, ASCII punctuation
        (0x2010, 0x2027),  # General Punctuation: dashes, quotes, ellipsis
        (0x0600, 0x06FF),  # Arabic
        (0x0750, 0x077F),  # Arabic Supplement
        (0xFDF2, 0xFDF2),  # Arabic Ligature Allah Isolated Form (Presentation Forms-A) --
                           # a single codepoint, not the full FB50-FDFF block, so ArabicShaper
                           # can special-case the four-letter "الله" sequence into one glyph.
        (0xFB56, 0xFB59),  # Peh forms (Persian)
        (0xFB7A, 0xFB7D),  # Tcheh forms (Persian)
        (0xFB8A, 0xFB95),  # Jeh/Keheh/Gaf forms (Persian; span includes Rreh)
        (0xFBA4, 0xFBA5),  # Heh with Yeh Above forms (Persian)
        (0xFBD3, 0xFBD6),  # Ng forms (Ottoman Turkish)
        (0xFBFC, 0xFBFF),  # Farsi Yeh forms (dotless-final yeh)
        (0xFD3E, 0xFD3F),  # Ornate parentheses -- Quran ayah-number markers, e.g. "﴿١٢٣﴾"
        (0xFE70, 0xFEFF),  # Arabic Presentation Forms-B
    ]
elif args.script == "none":
    intervals = []

add_ints = []
if args.additional_intervals:
    add_ints = [tuple([int(n, base=0) for n in i.split(",")]) for i in args.additional_intervals]

def norm_floor(val):
    return int(math.floor(val / (1 << 6)))

def norm_ceil(val):
    return int(math.ceil(val / (1 << 6)))

# Fixed-point (fp4) output conventions (must match EpdFontData.h / fp4 namespace):
#
#   advanceX    12.4 unsigned fixed-point (uint16_t).
#               12 integer bits, 4 fractional bits = 1/16-pixel resolution.
#               Encoded from FreeType's 16.16 linearHoriAdvance.
#
#   kernMatrix  4.4 signed fixed-point (int8_t).
#               4 integer bits, 4 fractional bits = 1/16-pixel resolution.
#               Range: -8.0 to +7.9375 pixels.
#               Encoded from font design-unit kerning values.
#
# Both share 4 fractional bits so the renderer can add them directly into a
# single int32_t accumulator and defer rounding until pixel placement.

def fp4_from_ft16_16(val):
    """Convert FreeType 16.16 fixed-point to 12.4 fixed-point with rounding."""
    return (val + (1 << 11)) >> 12

def fp4_from_design_units(du, scale):
    """Convert a font design-unit value to 4.4 fixed-point, clamped to int8_t.

    Multiplies by scale (ppem / units_per_em) and shifts into 4 fractional
    bits.  The result is rounded to nearest and clamped to [-128, 127].
    """
    raw = round(du * scale * 16)
    return max(-128, min(127, raw))

def chunks(l, n):
    for i in range(0, len(l), n):
        yield l[i:i + n]

def extract_pnum_subs(font_path):
    """Extract pnum (proportional figures) GSUB substitutions.

    Parses the font's GSUB table for the 'pnum' feature, which replaces
    tabular-width figure glyphs with proportional-width alternates.
    Returns {original_glyph_name: substitute_glyph_name} or empty dict.
    """
    font = TTFont(font_path)
    subs = {}
    if 'GSUB' not in font:
        font.close()
        return subs
    gsub = font['GSUB'].table
    pnum_indices = set()
    if gsub.FeatureList:
        for fr in gsub.FeatureList.FeatureRecord:
            if fr.FeatureTag == 'pnum':
                pnum_indices.update(fr.Feature.LookupListIndex)
    for li in pnum_indices:
        lookup = gsub.LookupList.Lookup[li]
        for st in lookup.SubTable:
            actual = st
            if lookup.LookupType == 7 and hasattr(st, 'ExtSubTable'):
                actual = st.ExtSubTable
            if hasattr(actual, 'mapping'):
                subs.update(actual.mapping)
    font.close()
    return subs

# Build proportional numeral glyph overrides when --pnum is active.
# Maps (face_index, codepoint) -> freetype glyph index for the proportional alternate.
pnum_glyph_overrides = {}
pnum_kern_subs = {}  # face_index -> {original_glyph_name: substitute_glyph_name}
if args.pnum:
    for face_idx, font_path in enumerate(args.fontstack):
        subs = extract_pnum_subs(font_path)
        if not subs:
            continue
        pnum_kern_subs[face_idx] = subs
        tt_font = TTFont(font_path)
        cmap = tt_font.getBestCmap() or {}
        glyph_order = tt_font.getGlyphOrder()
        name_to_glyph_idx = {name: idx for idx, name in enumerate(glyph_order)}
        count = 0
        for cp, glyph_name in cmap.items():
            if glyph_name in subs:
                sub_name = subs[glyph_name]
                sub_idx = name_to_glyph_idx.get(sub_name, 0)
                if sub_idx > 0:
                    pnum_glyph_overrides[(face_idx, cp)] = sub_idx
                    count += 1
        tt_font.close()
        if count > 0:
            print(f"pnum: {count} glyph substitutions from {font_path}", file=sys.stderr)

# --- Presentation-forms fallback via OpenType shaping (--shape-fallback) ---
# Quranic fonts often omit cmap entries for Arabic Presentation Forms (they map
# contextual forms only through GSUB, which the firmware's renderer doesn't
# run). For every presentation-form codepoint missing from the FIRST face's
# cmap whose Unicode compatibility decomposition is tagged <isolated>/<final>/
# <initial>/<medial>, shape its base letter(s) with HarfBuzz in the matching
# join context (ZWJ neighbours) and record the resulting glyph index as a
# load_glyph() override -- the rest of the pipeline (metrics, rasterization,
# kerning) then treats it like any cmap-mapped glyph.
if args.shape_fallback:
    import unicodedata
    import uharfbuzz as hb

    _hb_blob = hb.Blob.from_file_path(args.fontstack[0])
    _hb_font = hb.Font(hb.Face(_hb_blob))
    ZWJ = "\u200d"
    _join_context = {
        "<isolated>": ("", ""),
        "<final>": (ZWJ, ""),
        "<initial>": ("", ZWJ),
        "<medial>": (ZWJ, ZWJ),
    }
    _face0 = font_stack[0]
    _fallback_count = 0
    for _cp in list(range(0xFB50, 0xFE00)) + list(range(0xFE70, 0xFF00)):
        if _face0.get_char_index(_cp) > 0:
            continue
        _d = unicodedata.decomposition(chr(_cp))
        if not _d.startswith("<"):
            continue
        _tag, _, _rest = _d.partition("> ")
        _tag += ">"
        if _tag not in _join_context:
            continue
        _base = "".join(chr(int(h, 16)) for h in _rest.split())
        _pre, _post = _join_context[_tag]
        _buf = hb.Buffer()
        _buf.add_str(_pre + _base + _post)
        _buf.guess_segment_properties()
        hb.shape(_hb_font, _buf)
        # ZWJ padding glyphs must be filtered by rendered width, not by cluster index
        # or by matching HarfBuzz's cmap-nominal glyph for U+200D: the Arabic shaper
        # merges the joiner's cluster into the base letter's cluster (so cluster-range
        # filtering can't tell them apart), and GSUB can substitute the joiner into a
        # different glyph than its nominal one (observed on KFGQPC Uthmanic Hafs: the
        # padding glyph came out as glyph "space", not the joiner's own nominal glyph
        # -- silently breaking the cluster/codepoint filter and skipping synthesis for
        # nearly every initial/medial letter form, so entire letters vanished mid-word).
        # A padding joiner is always zero-width; the real letterform never is.
        _gids = [g.codepoint for g, p in zip(_buf.glyph_infos, _buf.glyph_positions)
                 if g.codepoint != 0 and (p.x_advance != 0 or p.y_advance != 0)]
        if len(_gids) != 1:
            continue  # only single-glyph results are unambiguous (ligatures collapse to one)
        pnum_glyph_overrides[(0, _cp)] = _gids[0]
        _fallback_count += 1
    print(f"shape-fallback: synthesized {_fallback_count} presentation-form glyphs via GSUB", file=sys.stderr)

# --- Explicit trigger-string glyph mapping via OpenType shaping (--glyph-map) ---
# For fonts whose glyphs are reachable only by shaping a specific (often
# non-Unicode, ASCII trigger) string through GSUB -- e.g. KFGQPC's
# surah-name-v4.ttf, which has no cmap entries at all and resolves a calligraphic
# surah name only by shaping "surah018surah-icon" through the font's own rules.
# Unlike --shape-fallback (which derives its trigger from Unicode decomposition
# data), the caller supplies the exact trigger string and which glyph in the
# shaped output to keep, since there's no general rule to derive either from the
# target codepoint alone.
if args.glyph_map:
    import uharfbuzz as hb

    _hb_blob_gm = hb.Blob.from_file_path(args.fontstack[0])
    _hb_font_gm = hb.Font(hb.Face(_hb_blob_gm))
    _glyph_map_count = 0
    with open(args.glyph_map, encoding="utf-8") as _f:
        for _lineno, _line in enumerate(_f, 1):
            _line = _line.rstrip("\n")
            if not _line or _line.startswith("#"):
                continue
            _parts = _line.split("\t")
            if len(_parts) != 3:
                print(f"glyph-map: {args.glyph_map}:{_lineno}: expected 3 tab-separated fields, got {len(_parts)}",
                      file=sys.stderr)
                sys.exit(1)
            _cp_gm = int(_parts[0], 16)
            _trigger_gm = _parts[1]
            _shaped_index = int(_parts[2])
            _buf_gm = hb.Buffer()
            _buf_gm.add_str(_trigger_gm)
            _buf_gm.guess_segment_properties()
            hb.shape(_hb_font_gm, _buf_gm)
            _gids_gm = [g.codepoint for g in _buf_gm.glyph_infos]
            if _shaped_index >= len(_gids_gm):
                print(f"glyph-map: {args.glyph_map}:{_lineno}: trigger {_trigger_gm!r} shaped to only "
                      f"{len(_gids_gm)} glyph(s), index {_shaped_index} out of range", file=sys.stderr)
                sys.exit(1)
            pnum_glyph_overrides[(0, _cp_gm)] = _gids_gm[_shaped_index]
            _glyph_map_count += 1
    print(f"glyph-map: mapped {_glyph_map_count} codepoints via {args.glyph_map}", file=sys.stderr)

def load_glyph(code_point):
    face_index = 0
    while face_index < len(font_stack):
        face = font_stack[face_index]
        glyph_index = pnum_glyph_overrides.get((face_index, code_point))
        if glyph_index is None:
            glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, load_flags)
            return face
        face_index += 1
    return None

unmerged_intervals = sorted(intervals + add_ints)
intervals = []
unvalidated_intervals = []
for i_start, i_end in unmerged_intervals:
    if len(unvalidated_intervals) > 0 and i_start <= unvalidated_intervals[-1][1] + 1:
        unvalidated_intervals[-1] = (unvalidated_intervals[-1][0], max(unvalidated_intervals[-1][1], i_end))
        continue
    unvalidated_intervals.append((i_start, i_end))

# Must run before validation (not after, as originally): EpdGlyph.width/height are
# uint8_t in the C++ struct, so validation needs to see bitmaps rasterized at our
# actual target size to catch any codepoint whose glyph is too big to fit before
# it ends up in `intervals` and reaches the C++ output as a > 255 literal (observed
# with KFGQPC Uthmanic Hafs's U+0600/U+0601 ARABIC NUMBER SIGN glyphs, which are
# wide decorative overlays spanning an entire following digit sequence -- not
# needed for body text, so simply excluding them like any other missing glyph is
# the right behavior, not a bug to work around glyph-by-glyph).
for face in font_stack:
    face.set_char_size(size << 6, size << 6, 150, 150)

MAX_GLYPH_DIM = 255  # EpdGlyph.width/height are uint8_t
oversized_warned = False
for i_start, i_end in unvalidated_intervals:
    start = i_start
    for code_point in range(i_start, i_end + 1):
        face = load_glyph(code_point)
        if face is not None and (face.glyph.bitmap.width > MAX_GLYPH_DIM or face.glyph.bitmap.rows > MAX_GLYPH_DIM):
            if not oversized_warned:
                print(f"WARNING: skipping oversized glyph(s) (> {MAX_GLYPH_DIM}px, doesn't fit EpdGlyph's uint8_t "
                      f"width/height) starting at U+{code_point:04X} "
                      f"({face.glyph.bitmap.width}x{face.glyph.bitmap.rows})", file=sys.stderr)
                oversized_warned = True
            face = None
        if face is None:
            if start < code_point:
                intervals.append((start, code_point - 1))
            start = code_point + 1
    if start != i_end + 1:
        intervals.append((start, i_end))

total_size = 0
all_glyphs = []

for i_start, i_end in intervals:
    for code_point in range(i_start, i_end + 1):
        face = load_glyph(code_point)
        bitmap = face.glyph.bitmap

        # Build out 4-bit greyscale bitmap
        pixels4g = []
        px = 0
        for i, v in enumerate(bitmap.buffer):
            if args.contrast_gamma != 1.0:
                # Boost raw AA coverage before any downsampling: --2bit only keeps
                # 4 levels, so a font whose strokes rasterize as mostly mid-gray
                # coverage (thin calligraphic hairlines at small e-ink sizes) reads
                # as faded once quantized. Applied to the coverage byte only --
                # never touches bitmap_left/top or linearHoriAdvance below, so this
                # can't perturb glyph metrics, advance widths, or kashida-point math.
                v = min(255, round(255 * (v / 255.0) ** args.contrast_gamma))
            y = i / bitmap.width
            x = i % bitmap.width
            if x % 2 == 0:
                px = (v >> 4)
            else:
                px = px | (v & 0xF0)
                pixels4g.append(px);
                px = 0
            # eol
            if x == bitmap.width - 1 and bitmap.width % 2 > 0:
                pixels4g.append(px)
                px = 0

        if is2Bit:
            # 0-3 white, 4-7 light grey, 8-11 dark grey, 12-15 black
            # Downsample to 2-bit bitmap
            pixels2b = []
            px = 0
            pitch = (bitmap.width // 2) + (bitmap.width % 2)
            for y in range(bitmap.rows):
                for x in range(bitmap.width):
                    px = px << 2
                    bm = pixels4g[y * pitch + (x // 2)]
                    bm = (bm >> ((x % 2) * 4)) & 0xF

                    if bm >= 12:
                        px += 3
                    elif bm >= 8:
                        px += 2
                    elif bm >= 4:
                        px += 1

                    if (y * bitmap.width + x) % 4 == 3:
                        pixels2b.append(px)
                        px = 0
            if (bitmap.width * bitmap.rows) % 4 != 0:
                px = px << (4 - (bitmap.width * bitmap.rows) % 4) * 2
                pixels2b.append(px)

            # for y in range(bitmap.rows):
            #     line = ''
            #     for x in range(bitmap.width):
            #         pixelPosition = y * bitmap.width + x
            #         byte = pixels2b[pixelPosition // 4]
            #         bit_index = (3 - (pixelPosition % 4)) * 2
            #         line += '#' if ((byte >> bit_index) & 3) > 0 else '.'
            #     print(line)
            # print('')
        else:
            # Downsample to 1-bit bitmap - treat any 2+ as black
            pixelsbw = []
            px = 0
            pitch = (bitmap.width // 2) + (bitmap.width % 2)
            for y in range(bitmap.rows):
                for x in range(bitmap.width):
                    px = px << 1
                    bm = pixels4g[y * pitch + (x // 2)]
                    px += 1 if ((x & 1) == 0 and bm & 0xE > 0) or ((x & 1) == 1 and bm & 0xE0 > 0) else 0

                    if (y * bitmap.width + x) % 8 == 7:
                        pixelsbw.append(px)
                        px = 0
            if (bitmap.width * bitmap.rows) % 8 != 0:
                px = px << (8 - (bitmap.width * bitmap.rows) % 8)
                pixelsbw.append(px)

            # for y in range(bitmap.rows):
            #     line = ''
            #     for x in range(bitmap.width):
            #         pixelPosition = y * bitmap.width + x
            #         byte = pixelsbw[pixelPosition // 8]
            #         bit_index = 7 - (pixelPosition % 8)
            #         line += '#' if (byte >> bit_index) & 1 else '.'
            #     print(line)
            # print('')

        pixels = pixels2b if is2Bit else pixelsbw

        # --reposition-marks: GPOS-reliant fonts anchor combining marks at the
        # baseline; without mark attachment they'd draw on top of the letter
        # bodies (observed with Neirizi: harakat invisible). Move them to the
        # fixed heights statically-positioned fonts use: above-marks just over
        # the tallest letters, below-marks just under the baseline. Targets
        # are em-relative (Noto Naskh carries fatha at ~0.89 em, kasra just
        # below baseline).
        #
        # A single fixed "above" height isn't enough for fully-vocalized Quranic
        # text: shadda commonly stacks with a vowel mark on the SAME letter (e.g.
        # "الرَّحْمَٰنِ" -- confirmed via corpus scan: shadda+fatha alone occurs
        # 1300+ times just in the first 20 of 114 surahs), and both marks would
        # land at the identical position and collide. Since this height is baked
        # per-CODEPOINT at conversion time (not computed from the actual runtime
        # sequence), the fix is a small ordered set of tiers keyed to each mark's
        # canonical stacking role (shadda always innermost, vowels stack outward
        # from it, Quranic small-high annotation marks/maddah outermost) rather
        # than a single shared height -- using Unicode's own canonical combining
        # class (unicodedata.combining()) to classify each mark programmatically.
        glyph_top = face.glyph.bitmap_top
        glyph_left = face.glyph.bitmap_left
        if args.reposition_marks and unicodedata.category(chr(code_point)) == 'Mn':
            em_px = size * 150 / 72.0
            below_marks = {0x0650, 0x064D, 0x0655, 0x0656, 0x065C}  # kasra, kasratan, hamza/subscript below
            ccc = unicodedata.combining(chr(code_point))
            if code_point in below_marks:
                glyph_top = -max(1, round(em_px * 0.05))
            elif code_point == 0x0651:  # shadda: innermost above-mark
                glyph_top = round(em_px * 0.75)
            elif ccc in (27, 28, 30, 31, 34, 35):  # tanwin/fatha/damma/sukun/dagger-alef
                glyph_top = round(em_px * 0.9)
            else:  # outer: maddah, hamza-above, Quranic small-high annotation marks
                glyph_top = round(em_px * 1.05)
            # Horizontal centering is done at RUNTIME (GfxRenderer::drawArabicText,
            # via combiningMark::centerOver against the mark's actual base letter) --
            # it depends on the base letter's width, which varies letter to letter and
            # can't be baked per-mark-codepoint here. Keep the font's natural
            # bitmap_left (small, near zero) as centerOver's own markLeft term.

        # Build output data
        packed = bytes(pixels)
        glyph = GlyphProps(
            width = bitmap.width,
            height = bitmap.rows,
            # We use linearHoriAdvance (16.16 fixed-point, unhinted) instead of
            # advance.x (26.6 fixed-point, grid-fitted to whole pixels by hinter)
            advance_x = fp4_from_ft16_16(face.glyph.linearHoriAdvance),
            left = glyph_left,
            top = glyph_top,
            data_length = len(packed),
            data_offset = total_size,
            code_point = code_point,
        )
        total_size += len(packed)
        all_glyphs.append((glyph, packed))

# pipe seems to be a good heuristic for the "real" descender
face = load_glyph(ord('|'))

glyph_data = []
glyph_props = []
for index, glyph in enumerate(all_glyphs):
    props, packed = glyph
    glyph_data.extend([b for b in packed])
    glyph_props.append(props)

# --- Kerning pair extraction ---
# Modern fonts store kerning in the OpenType GPOS table, which FreeType's
# get_kerning() does not read. We use fonttools to parse both the legacy
# kern table and the GPOS 'kern' feature (PairPos lookups, including
# Extension wrappers).

COMBINING_MARKS_START = 0x0300
COMBINING_MARKS_END = 0x036F
all_codepoints = [g.code_point for g in glyph_props]
kernable_codepoints = set(cp for cp in all_codepoints
                          if not (COMBINING_MARKS_START <= cp <= COMBINING_MARKS_END))

# Map each kernable codepoint to the font-stack index that serves it
# (same priority logic as load_glyph).
cp_to_face_idx = {}
for cp in kernable_codepoints:
    for face_idx, f in enumerate(font_stack):
        if f.get_char_index(cp) > 0:
            cp_to_face_idx[cp] = face_idx
            break

# Group codepoints by face index
face_idx_cps = {}
for cp, fi in cp_to_face_idx.items():
    face_idx_cps.setdefault(fi, set()).add(cp)

def _extract_pairpos_subtable(subtable, glyph_to_cp, raw_kern):
    """Extract kerning from a PairPos subtable (Format 1 or 2)."""
    if subtable.Format == 1:
        # Individual pairs
        for i, coverage_glyph in enumerate(subtable.Coverage.glyphs):
            if coverage_glyph not in glyph_to_cp:
                continue
            pair_set = subtable.PairSet[i]
            for pvr in pair_set.PairValueRecord:
                if pvr.SecondGlyph not in glyph_to_cp:
                    continue
                xa = 0
                if hasattr(pvr, 'Value1') and pvr.Value1:
                    xa = getattr(pvr.Value1, 'XAdvance', 0) or 0
                if xa != 0:
                    key = (coverage_glyph, pvr.SecondGlyph)
                    raw_kern[key] = raw_kern.get(key, 0) + xa
    elif subtable.Format == 2:
        # Class-based pairs. Some fonts' PairPos Format 2 subtables (seen in Amiri,
        # Scheherazade New) omit fields fontTools normally expects here (ClassDef1,
        # ClassDef2, even Class1Record itself) -- direct attribute access raises
        # AttributeError rather than returning None/empty, unlike Value1 below.
        # Kerning is a nice-to-have refinement, not essential correctness, so skip
        # a subtable we can't parse instead of aborting the whole font conversion.
        try:
            class_def1_table = getattr(subtable, 'ClassDef1', None)
            class_def2_table = getattr(subtable, 'ClassDef2', None)
            class_def1 = class_def1_table.classDefs if class_def1_table else {}
            class_def2 = class_def2_table.classDefs if class_def2_table else {}
            class1_records = subtable.Class1Record
        except AttributeError:
            return
        coverage_set = set(subtable.Coverage.glyphs)
        for left_glyph in glyph_to_cp:
            if left_glyph not in coverage_set:
                continue
            c1 = class_def1.get(left_glyph, 0)
            if c1 >= len(class1_records):
                continue
            class1_rec = class1_records[c1]
            for right_glyph in glyph_to_cp:
                c2 = class_def2.get(right_glyph, 0)
                if c2 >= len(class1_rec.Class2Record):
                    continue
                c2_rec = class1_rec.Class2Record[c2]
                xa = 0
                if hasattr(c2_rec, 'Value1') and c2_rec.Value1:
                    xa = getattr(c2_rec.Value1, 'XAdvance', 0) or 0
                if xa != 0:
                    key = (left_glyph, right_glyph)
                    raw_kern[key] = raw_kern.get(key, 0) + xa

def extract_kerning_fonttools(font_path, codepoints, ppem, pnum_subs=None):
    """Extract kerning pairs from a font file using fonttools.

    Returns dict of {(leftCp, rightCp): pixel_adjust} for the given
    codepoints.  Values are scaled from font design units to integer
    pixels at ppem.

    When pnum_subs is provided, substitute glyph names are also included
    in the lookup so kern pairs referencing proportional alternates are found.
    """
    font = TTFont(font_path)
    units_per_em = font['head'].unitsPerEm
    cmap = font.getBestCmap() or {}

    # Build glyph_name -> codepoint map (only for requested codepoints).
    # When pnum is active, include both the original and substitute glyph
    # names so kern pairs referencing either are captured.
    glyph_to_cp = {}
    for cp in codepoints:
        gname = cmap.get(cp)
        if gname:
            glyph_to_cp[gname] = cp
            if pnum_subs and gname in pnum_subs:
                glyph_to_cp[pnum_subs[gname]] = cp

    # Collect raw kerning values in font design units
    raw_kern = {}  # (left_glyph_name, right_glyph_name) -> design_units

    # 1. Legacy kern table
    if 'kern' in font:
        for subtable in font['kern'].kernTables:
            if hasattr(subtable, 'kernTable'):
                for (lg, rg), val in subtable.kernTable.items():
                    if lg in glyph_to_cp and rg in glyph_to_cp:
                        raw_kern[(lg, rg)] = raw_kern.get((lg, rg), 0) + val

    # 2. GPOS 'kern' feature
    if 'GPOS' in font:
        gpos = font['GPOS'].table
        kern_lookup_indices = set()
        if gpos.FeatureList:
            for fr in gpos.FeatureList.FeatureRecord:
                if fr.FeatureTag == 'kern':
                    kern_lookup_indices.update(fr.Feature.LookupListIndex)
        for li in kern_lookup_indices:
            lookup = gpos.LookupList.Lookup[li]
            for st in lookup.SubTable:
                actual = st
                # Unwrap Extension (lookup type 9) wrappers
                if lookup.LookupType == 9 and hasattr(st, 'ExtSubTable'):
                    actual = st.ExtSubTable
                if hasattr(actual, 'Format'):
                    _extract_pairpos_subtable(actual, glyph_to_cp, raw_kern)

    font.close()

    # Scale design-unit kerning values to 4.4 fixed-point pixels.
    scale = ppem / units_per_em
    result = {}  # (leftCp, rightCp) -> 4.4 fixed-point adjust
    for (lg, rg), du in raw_kern.items():
        lcp = glyph_to_cp[lg]
        rcp = glyph_to_cp[rg]
        adjust = fp4_from_design_units(du, scale)
        if adjust != 0:
            result[(lcp, rcp)] = adjust
    return result

# The ppem used by the existing glyph rasterization:
#   face.set_char_size(size << 6, size << 6, 150, 150)
# means size_pt at 150 DPI -> ppem = size * 150 / 72
ppem = size * 150.0 / 72.0

kern_map = {}  # (leftCp, rightCp) -> adjust
for face_idx, cps in face_idx_cps.items():
    font_path = args.fontstack[face_idx]
    subs = pnum_kern_subs.get(face_idx) if args.pnum else None
    kern_map.update(extract_kerning_fonttools(font_path, cps, ppem, pnum_subs=subs))

print(f"kerning: {len(kern_map)} pairs extracted", file=sys.stderr)

# --- Derive class-based kerning from pairs ---
kern_left_classes = []   # list of (codepoint, classId)
kern_right_classes = []  # list of (codepoint, classId)
kern_matrix = []         # flat list of int8_t values
kern_left_class_count = 0
kern_right_class_count = 0

if kern_map:
    all_left_cps = {lcp for lcp, _ in kern_map}
    all_right_cps = {rcp for _, rcp in kern_map}

    sorted_right_cps = sorted(all_right_cps)
    sorted_left_cps = sorted(all_left_cps)

    # Group left codepoints by identical adjustment row
    left_profile_to_class = {}
    left_class_map = {}
    left_class_id = 1
    for lcp in sorted(all_left_cps):
        row = tuple(kern_map.get((lcp, rcp), 0) for rcp in sorted_right_cps)
        if row not in left_profile_to_class:
            left_profile_to_class[row] = left_class_id
            left_class_id += 1
        left_class_map[lcp] = left_profile_to_class[row]

    # Group right codepoints by identical adjustment column
    right_profile_to_class = {}
    right_class_map = {}
    right_class_id = 1
    for rcp in sorted(all_right_cps):
        col = tuple(kern_map.get((lcp, rcp), 0) for lcp in sorted_left_cps)
        if col not in right_profile_to_class:
            right_profile_to_class[col] = right_class_id
            right_class_id += 1
        right_class_map[rcp] = right_profile_to_class[col]

    kern_left_class_count = left_class_id - 1
    kern_right_class_count = right_class_id - 1

    if kern_left_class_count > 255 or kern_right_class_count > 255:
        print(f"WARNING: kerning class count exceeds uint8_t range "
              f"(left={kern_left_class_count}, right={kern_right_class_count})",
              file=sys.stderr)

    # Build the class x class matrix
    kern_matrix = [0] * (kern_left_class_count * kern_right_class_count)
    for (lcp, rcp), adjust in kern_map.items():
        lc = left_class_map[lcp] - 1
        rc = right_class_map[rcp] - 1
        kern_matrix[lc * kern_right_class_count + rc] = adjust

    # Build sorted class entry lists
    kern_left_classes = sorted(left_class_map.items())
    kern_right_classes = sorted(right_class_map.items())

    matrix_size = kern_left_class_count * kern_right_class_count
    entries_size = (len(kern_left_classes) + len(kern_right_classes)) * 3
    print(f"kerning: {kern_left_class_count} left classes, {kern_right_class_count} right classes, "
          f"{matrix_size + entries_size} bytes", file=sys.stderr)

# --- Ligature pair extraction ---
# Parse the OpenType GSUB table for LigatureSubst (type 4) lookups.
# Multi-character ligatures (3+ codepoints) are decomposed into chained
# pairs when an intermediate ligature exists (e.g., ffi = ff + i where ff
# is itself a ligature). Only pairs where both input codepoints and the
# output codepoint are in the generated glyph set are included.

all_codepoints_set = set(all_codepoints)

# Standard Unicode ligature codepoints for known input sequences.
# Used as a fallback when the GSUB substitute glyph has no cmap entry.
STANDARD_LIGATURE_MAP = {
    (0x66, 0x66):       0xFB00,  # ff
    (0x66, 0x69):       0xFB01,  # fi
    (0x66, 0x6C):       0xFB02,  # fl
    (0x66, 0x66, 0x69): 0xFB03,  # ffi
    (0x66, 0x66, 0x6C): 0xFB04,  # ffl
    (0x17F, 0x74):      0xFB05,  # long-s + t
    (0x73, 0x74):       0xFB06,  # st
}

def extract_ligatures_fonttools(font_path, codepoints):
    """Extract ligature substitution pairs from a font file using fonttools.

    Returns list of (packed_pair, ligature_codepoint) for the given codepoints.
    Multi-character ligatures are decomposed into chained pairs.
    """
    font = TTFont(font_path)
    cmap = font.getBestCmap() or {}

    # Build glyph_name -> codepoint and codepoint -> glyph_name maps
    glyph_to_cp = {}
    cp_to_glyph = {}
    for cp, gname in cmap.items():
        glyph_to_cp[gname] = cp
        cp_to_glyph[cp] = gname

    # Collect raw ligature rules: (sequence_of_codepoints) -> ligature_codepoint
    raw_ligatures = {}  # tuple of codepoints -> ligature codepoint

    if 'GSUB' in font:
        gsub = font['GSUB'].table

        # Find lookup indices for ligature features.
        # Currently extracts 'liga' (standard) and 'rlig' (required) only.
        # To also extract discretionary or historical ligatures, add:
        #   'dlig' - Discretionary Ligatures (e.g., ft, st in Noto)
        #   'hlig' - Historical Ligatures (e.g., long-s+t in OpenDyslexic)
        # These are off by default in standard text renderers.
        LIGATURE_FEATURES = ('liga', 'rlig')
        liga_lookup_indices = set()
        if gsub.FeatureList:
            for fr in gsub.FeatureList.FeatureRecord:
                if fr.FeatureTag in LIGATURE_FEATURES:
                    liga_lookup_indices.update(fr.Feature.LookupListIndex)

        for li in liga_lookup_indices:
            lookup = gsub.LookupList.Lookup[li]
            for st in lookup.SubTable:
                actual = st
                # Unwrap Extension (lookup type 7) wrappers
                if lookup.LookupType == 7 and hasattr(st, 'ExtSubTable'):
                    actual = st.ExtSubTable
                # LigatureSubst is lookup type 4
                if not hasattr(actual, 'ligatures'):
                    continue
                for first_glyph, ligature_list in actual.ligatures.items():
                    if first_glyph not in glyph_to_cp:
                        continue
                    first_cp = glyph_to_cp[first_glyph]
                    for lig in ligature_list:
                        # lig.Component is a list of subsequent glyph names
                        # lig.LigGlyph is the substitute glyph name
                        component_cps = []
                        valid = True
                        for comp_glyph in lig.Component:
                            if comp_glyph not in glyph_to_cp:
                                valid = False
                                break
                            component_cps.append(glyph_to_cp[comp_glyph])
                        if not valid:
                            continue
                        seq = tuple([first_cp] + component_cps)
                        if lig.LigGlyph in glyph_to_cp:
                            lig_cp = glyph_to_cp[lig.LigGlyph]
                        elif seq in STANDARD_LIGATURE_MAP:
                            lig_cp = STANDARD_LIGATURE_MAP[seq]
                        else:
                            seq_str = ', '.join(f'U+{cp:04X}' for cp in seq)
                            print(f"ligatures: WARNING: dropping ligature ({seq_str}) -> "
                                  f"glyph '{lig.LigGlyph}': output glyph has no cmap entry "
                                  f"and input sequence is not in STANDARD_LIGATURE_MAP",
                                  file=sys.stderr)
                            continue
                        raw_ligatures[seq] = lig_cp

    font.close()

    # Filter: only keep ligatures where all input and output codepoints are
    # in our generated glyph set
    filtered = {}
    for seq, lig_cp in raw_ligatures.items():
        if lig_cp not in codepoints and lig_cp not in all_codepoints_set:
            continue
        if all(cp in codepoints for cp in seq):
            filtered[seq] = lig_cp

    # Decompose into chained pairs
    # For 2-codepoint sequences: direct pair (a, b) -> lig
    # For 3+ codepoint sequences: chain through intermediates
    #   e.g., (f, f, i) -> ffi requires (f, f) -> ff to exist,
    #   then we add (ff, i) -> ffi
    pairs = []
    # First pass: collect all 2-codepoint ligatures
    two_char = {seq: lig_cp for seq, lig_cp in filtered.items() if len(seq) == 2}
    for seq, lig_cp in two_char.items():
        packed = (seq[0] << 16) | seq[1]
        pairs.append((packed, lig_cp))

    # Second pass: decompose 3+ codepoint ligatures into chained pairs
    for seq, lig_cp in filtered.items():
        if len(seq) < 3:
            continue
        # Try to find an intermediate: check if the first N-1 codepoints
        # form a known ligature, then chain (intermediate, last) -> lig
        prefix = seq[:-1]
        last_cp = seq[-1]
        if prefix in filtered:
            intermediate_cp = filtered[prefix]
            packed = (intermediate_cp << 16) | last_cp
            pairs.append((packed, lig_cp))
        else:
            print(f"ligatures: skipping {len(seq)}-char ligature "
                  f"({', '.join(f'U+{cp:04X}' for cp in seq)}) -> U+{lig_cp:04X}: "
                  f"no intermediate ligature for prefix", file=sys.stderr)

    return pairs

ligature_codepoints = set(cp for cp in all_codepoints
                          if not (COMBINING_MARKS_START <= cp <= COMBINING_MARKS_END))

# Map ligature codepoints to the font-stack index that serves them
lig_cp_to_face_idx = {}
for cp in ligature_codepoints:
    for face_idx, f in enumerate(font_stack):
        if f.get_char_index(cp) > 0:
            lig_cp_to_face_idx[cp] = face_idx
            break

# Group by face index
lig_face_idx_cps = {}
for cp, fi in lig_cp_to_face_idx.items():
    lig_face_idx_cps.setdefault(fi, set()).add(cp)

ligature_pairs = []
for face_idx, cps in lig_face_idx_cps.items():
    font_path = args.fontstack[face_idx]
    ligature_pairs.extend(extract_ligatures_fonttools(font_path, cps))

# Deduplicate (keep first occurrence) and sort
seen_lig_keys = set()
unique_ligature_pairs = []
for packed, lig_cp in ligature_pairs:
    if packed not in seen_lig_keys:
        seen_lig_keys.add(packed)
        unique_ligature_pairs.append((packed, lig_cp))
ligature_pairs = sorted(unique_ligature_pairs, key=lambda p: p[0])
print(f"ligatures: {len(ligature_pairs)} pairs extracted", file=sys.stderr)

compress = args.compress


def to_byte_aligned(packed, width, height):
    """Convert packed 2-bit bitmap to byte-aligned format (rows padded to byte boundary).

    In packed format, pixels flow continuously across row boundaries (4 pixels/byte).
    In byte-aligned format, each row starts at a byte boundary, padding the last byte
    of each row with zero bits if width % 4 != 0. This improves DEFLATE compression
    because identical pixel rows produce identical byte patterns regardless of position.
    """
    if width == 0 or height == 0:
        return b''
    row_stride = (width + 3) // 4  # bytes per byte-aligned row
    aligned = bytearray(row_stride * height)
    for y in range(height):
        for x in range(width):
            # Read pixel from packed format (continuous bit stream)
            packed_pos = y * width + x
            packed_byte_idx = packed_pos // 4
            packed_shift = (3 - (packed_pos % 4)) * 2
            pixel = (packed[packed_byte_idx] >> packed_shift) & 0x3

            # Write pixel to byte-aligned format (row-aligned)
            aligned_byte_idx = y * row_stride + x // 4
            aligned_shift = (3 - (x % 4)) * 2
            aligned[aligned_byte_idx] |= (pixel << aligned_shift)
    return bytes(aligned)


# Build groups for compression
if compress and not is2Bit:
    print("Error: --compress requires --2bit (byte-aligned compression only supports 2-bit format)", file=sys.stderr)
    sys.exit(1)
if compress:
    # Script-based grouping: glyphs that co-occur in typical text rendering
    # are grouped together for efficient LRU caching on the embedded target.
    # Since glyphs are in codepoint order, glyphs in the same Unicode block
    # are contiguous in the array and form natural groups.
    #
    # On top of script boundaries, a hard size cap (GROUP_MAX_UNCOMPRESSED_BYTES)
    # is applied: if adding the next glyph would push the uncompressed group
    # size over the cap, the group is closed and a new one started with the
    # same script ID. This bounds the embedded decompressor's transient
    # malloc regardless of font density (CJK, Vietnamese, user-supplied
    # fonts with large Unicode blocks). Without it, a single dense script
    # group can balloon past what fits in a transient page-decompress
    # allocation on the device.
    SCRIPT_GROUP_RANGES = [
        (0x0000, 0x007F),   # ASCII
        (0x0080, 0x00FF),   # Latin-1 Supplement
        (0x0100, 0x017F),   # Latin Extended-A
        (0x0180, 0x024F),   # Latin Extended-B
        (0x0300, 0x036F),   # Combining Diacritical Marks
        (0x0400, 0x04FF),   # Cyrillic
        (0x0600, 0x06FF),   # Arabic
        (0x0750, 0x077F),   # Arabic Supplement
        (0x08A0, 0x08FF),   # Arabic Extended-A
        (0x1EA0, 0x1EF9),   # Vietnamese Extended
        (0x2000, 0x206F),   # General Punctuation
        (0x2070, 0x209F),   # Superscripts & Subscripts
        (0x20A0, 0x20CF),   # Currency Symbols
        (0x2190, 0x21FF),   # Arrows
        (0x2200, 0x22FF),   # Math Operators
        (0xFB00, 0xFB06),   # Alphabetic Presentation Forms (ligatures)
        (0xFB50, 0xFDFF),   # Arabic Presentation Forms-A
        (0xFE70, 0xFEFF),   # Arabic Presentation Forms-B
        (0xFFFD, 0xFFFD),   # Replacement Character
    ]

    # 12 KB cap (was 64 KB): a fully-vocalized Arabic font has no script-range split
    # above (Arabic script simply wasn't in SCRIPT_GROUP_RANGES before this fix), so
    # every Arabic glyph fell into one "unknown script" bucket capped only by this
    # constant -- UthmanicHafs 18pt ended up with a single 414-glyph, 47924-byte
    # group. Real-device logs (bitmap_fail == misses almost 1:1, firstFailedAllocBytes
    # == 47924, reported free heap 75-90KB) showed prewarmCache()'s per-group temp
    # buffer (and getBitmap()'s hot-group fallback, same size) regularly failing to
    # allocate that ~48KB in one contiguous block despite the reported free heap
    # being nominally larger -- fragmentation, not exhaustion, per FontDecompressor's
    # own firstFailedAllocBytes-vs-heap comparison. Silent failure mode: every
    # caller treats a failed getBitmap() as "skip this glyph" with no visible error,
    # so the page just renders with entire lines of body text missing (only markers
    # drawn through other fonts, like the ayah-number glyph, survive). 12 KB keeps
    # each group's transient malloc comfortably inside even a moderately fragmented
    # ~75 KB heap; the added Arabic script ranges above additionally keep related
    # contextual/positional forms grouped together rather than split arbitrarily.
    GROUP_MAX_UNCOMPRESSED_BYTES = 12288

    def get_script_group(code_point):
        for i, (start, end) in enumerate(SCRIPT_GROUP_RANGES):
            if start <= code_point <= end:
                return i
        return -1

    groups = []  # list of (first_glyph_index, glyph_count)
    current_group_id = None
    group_start = 0
    group_count = 0
    group_uncompressed = 0

    for i, (props, _) in enumerate(all_glyphs):
        sg = get_script_group(props.code_point)
        # Use the byte-aligned size (4-pixel-aligned row stride) rather than
        # the packed length, since the decompressor consumes byte-aligned
        # buffers. Empty glyphs contribute zero.
        glyph_aligned_size = (((props.width + 3) // 4) * props.height
                              if props.width > 0 and props.height > 0 else 0)
        if glyph_aligned_size > GROUP_MAX_UNCOMPRESSED_BYTES:
            raise ValueError(
                f"Glyph {i} (code point U+{props.code_point:04X}) byte-aligned size "
                f"{glyph_aligned_size} exceeds GROUP_MAX_UNCOMPRESSED_BYTES="
                f"{GROUP_MAX_UNCOMPRESSED_BYTES}. Consider: (1) increasing GROUP_MAX_UNCOMPRESSED_BYTES, "
                f"(2) reducing font size, or (3) excluding this codepoint."  
            )
        size_overflow = group_uncompressed + glyph_aligned_size > GROUP_MAX_UNCOMPRESSED_BYTES

        if sg != current_group_id or size_overflow:
            if group_count > 0:
                groups.append((group_start, group_count))
            current_group_id = sg
            group_start = i
            group_count = 1
            group_uncompressed = glyph_aligned_size
        else:
            group_count += 1
            group_uncompressed += glyph_aligned_size

    if group_count > 0:
        groups.append((group_start, group_count))

    # Compress each group
    compressed_groups = []  # list of (compressed_bytes, uncompressed_size, glyph_count, first_glyph_index)
    compressed_bitmap_data = []
    compressed_offset = 0

    # Also build modified glyph props with within-group offsets
    modified_glyph_props = list(glyph_props)

    for first_idx, count in groups:
        # Concatenate bitmap data for this group
        packed_len = 0
        group_aligned = bytearray()
        for gi in range(first_idx, first_idx + count):
            props, packed = all_glyphs[gi]
            # Update glyph's dataOffset to be within-group offset (packed offset)
            within_group_offset = packed_len
            old_props = modified_glyph_props[gi]
            modified_glyph_props[gi] = GlyphProps(
                width=old_props.width,
                height=old_props.height,
                advance_x=old_props.advance_x,
                left=old_props.left,
                top=old_props.top,
                data_length=old_props.data_length,
                data_offset=within_group_offset,
                code_point=old_props.code_point,
            )
            packed_len += len(packed)
            group_aligned.extend(to_byte_aligned(packed, old_props.width, old_props.height))

        # Compress byte-aligned data with raw DEFLATE (no zlib/gzip header)
        compressor = zlib.compressobj(level=9, wbits=-15)
        compressed = compressor.compress(bytes(group_aligned)) + compressor.flush()

        compressed_groups.append((compressed, len(group_aligned), count, first_idx))
        compressed_bitmap_data.extend(compressed)
        compressed_offset += len(compressed)

    glyph_props = modified_glyph_props
    total_compressed = len(compressed_bitmap_data)
    total_uncompressed = len(glyph_data)
    print(f"// Compression: {total_uncompressed} -> {total_compressed} bytes ({100*total_compressed/total_uncompressed:.1f}%), {len(groups)} groups", file=sys.stderr)

print(f"""/**
 * generated by fontconvert.py
 * name: {font_name}
 * size: {size}
 * mode: {'2-bit' if is2Bit else '1-bit'}{'  compressed: true' if compress else ''}
 * Command used: {' '.join(sys.argv)}
 */
#pragma once
#include "EpdFontData.h"
""")

if compress:
    print(f"static const uint8_t {font_name}Bitmaps[{len(compressed_bitmap_data)}] = {{")
    for c in chunks(compressed_bitmap_data, 16):
        print ("    " + " ".join(f"0x{b:02X}," for b in c))
    print ("};\n");
else:
    print(f"static const uint8_t {font_name}Bitmaps[{len(glyph_data)}] = {{")
    for c in chunks(glyph_data, 16):
        print ("    " + " ".join(f"0x{b:02X}," for b in c))
    print ("};\n");

def cp_label(cp):
    if cp == 0x5C:
        return '<backslash>'
    return chr(cp) if 0x20 < cp < 0x7F else f'U+{cp:04X}'

print(f"static const EpdGlyph {font_name}Glyphs[] = {{")
for i, g in enumerate(glyph_props):
    print ("    { " + ", ".join([f"{a}" for a in list(g[:-1])]),"},", f"// {cp_label(g.code_point)}")
print ("};\n");

print(f"static const EpdUnicodeInterval {font_name}Intervals[] = {{")
offset = 0
for i_start, i_end in intervals:
    print (f"    {{ 0x{i_start:X}, 0x{i_end:X}, 0x{offset:X} }},")
    offset += i_end - i_start + 1
print ("};\n");

if compress:
    print(f"static const EpdFontGroup {font_name}Groups[] = {{")
    compressed_offset = 0
    for compressed, uncompressed_size, count, first_idx in compressed_groups:
        print(f"    {{ {compressed_offset}, {len(compressed)}, {uncompressed_size}, {count}, {first_idx} }},")
        compressed_offset += len(compressed)
    print("};\n")

if kern_map:
    print(f"static const EpdKernClassEntry {font_name}KernLeftClasses[] = {{")
    for cp, cls in kern_left_classes:
        print(f"    {{ 0x{cp:04X}, {cls} }}, // {cp_label(cp)}")
    print("};\n")

    print(f"static const EpdKernClassEntry {font_name}KernRightClasses[] = {{")
    for cp, cls in kern_right_classes:
        print(f"    {{ 0x{cp:04X}, {cls} }}, // {cp_label(cp)}")
    print("};\n")

    print(f"static const int8_t {font_name}KernMatrix[] = {{")
    for row in range(kern_left_class_count):
        row_start = row * kern_right_class_count
        row_vals = kern_matrix[row_start:row_start + kern_right_class_count]
        print("    " + ", ".join(f"{v:4d}" for v in row_vals) + ",")
    print("};\n")

if ligature_pairs:
    print(f"static const EpdLigaturePair {font_name}LigaturePairs[] = {{")
    for packed_pair, lig_cp in ligature_pairs:
        print(f"    {{ 0x{packed_pair:08X}, 0x{lig_cp:04X} }}, // {cp_label(packed_pair >> 16)} {cp_label(packed_pair & 0xFFFF)} -> {cp_label(lig_cp)}")
    print("};\n")

# Face-level metrics (height/ascender/descender) are identical across every face
# in font_stack once set_char_size() ran on all of them above -- use font_stack[0]
# directly rather than the `face` loop variable, which is left over from whichever
# codepoint the glyph-rendering loop processed last (or, if `intervals` ended up
# empty for this font, from the interval-validation loop instead, where it can be
# None -- e.g. a font with sparse enough coverage of a requested range).
print(f"static const EpdFontData {font_name} = {{")
print(f"    {font_name}Bitmaps,")
print(f"    {font_name}Glyphs,")
print(f"    {font_name}Intervals,")
print(f"    {len(intervals)},")
print(f"    {norm_ceil(font_stack[0].size.height)},")
print(f"    {norm_ceil(font_stack[0].size.ascender)},")
print(f"    {norm_floor(font_stack[0].size.descender)},")
print(f"    {'true' if is2Bit else 'false'},")
if compress:
    print(f"    {font_name}Groups,")
    print(f"    {len(compressed_groups)},")
else:
    print("    nullptr,")
    print("    0,")
# glyphToGroup (not used for script-grouped fonts)
print("    nullptr,")
if kern_map:
    print(f"    {font_name}KernLeftClasses,")
    print(f"    {font_name}KernRightClasses,")
    print(f"    {font_name}KernMatrix,")
    print(f"    {len(kern_left_classes)},")
    print(f"    {len(kern_right_classes)},")
    print(f"    {kern_left_class_count},")
    print(f"    {kern_right_class_count},")
else:
    print(f"    nullptr,")
    print(f"    nullptr,")
    print(f"    nullptr,")
    print(f"    0,")
    print(f"    0,")
    print(f"    0,")
    print(f"    0,")
if ligature_pairs:
    print(f"    {font_name}LigaturePairs,")
    print(f"    {len(ligature_pairs)},")
else:
    print(f"    nullptr,")
    print(f"    0,")
print("};")
