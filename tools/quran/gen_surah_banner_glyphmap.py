#!/usr/bin/env python3
"""Generates the --glyph-map TSV for surah-name-v4.ttf's calligraphic surah-name
glyphs (see lib/EpdFont/scripts/fontconvert.py's --glyph-map help text).

The font has no cmap entries at all -- each surah's calligraphy is reachable only
by shaping the ASCII trigger string "surah{NNN}surah-icon" through the font's own
GSUB rules, which resolves to exactly 2 glyphs: one surah-specific name glyph,
and one ornament glyph shared identically by all 114 surahs (verified directly via
uharfbuzz before writing this script). This maps each of the 114 name glyphs to
its own dedicated PUA codepoint, plus one line for the shared ornament.

Usage: python3 gen_surah_banner_glyphmap.py > surah_banner_glyphmap.tsv
"""
import sys

# Must match GfxRenderer.cpp's SURAH_BANNER_NAME_BASE_CP / SURAH_BANNER_ORNAMENT_CP.
NAME_BASE_CP = 0xE010  # surah 1's name glyph; surah n -> NAME_BASE_CP + (n - 1)
ORNAMENT_CP = 0xE010 + 114  # 0xE086, immediately after the 114th name codepoint


def main():
    for n in range(1, 115):
        trigger = f"surah{n:03d}surah-icon"
        cp = NAME_BASE_CP + (n - 1)
        print(f"{cp:04X}\t{trigger}\t0")
    # Any surah's trigger shapes the shared ornament at index 1; surah 1 is as
    # good as any (confirmed identical glyph id across all 114 in exploration).
    print(f"{ORNAMENT_CP:04X}\tsurah001surah-icon\t1")


if __name__ == "__main__":
    main()
