#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
NOTOSERIF_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)

for size in ${NOTOSERIF_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notoserif_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSerif/NotoSerif-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-${style}.ttf"
    # Ubuntu lacks the Latin Extended Additional block (U+1EA0-U+1EF9) used for
    # Vietnamese tone marks. Append a Vietnamese-only Ubuntu cut so those glyphs
    # are filled from it while every glyph Ubuntu already has stays unchanged
    # (fontstack is ordered by descending priority).
    viet_path="../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $hebrew_path $viet_path \
      --additional-intervals 0x05D0,0x05EA > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  --additional-intervals 0x05D0,0x05EA > ../builtinFonts/notosans_8_regular.h

# All three built-in Arabic fonts below have OpenType GPOS MarkBasePos/MarkMarkPos
# tables (confirmed via fontTools) -- they anchor tashkeel to their base letter
# through GPOS, which this repo's FreeType-based single-glyph rasterizer never
# runs. Without --reposition-marks each mark glyph rasterizes at its own raw,
# GPOS-relative origin instead of a sensible fixed position, which on real
# hardware showed up as tashkeel intermittently missing/misplaced (some raw
# offsets happened to land somewhere visible, most didn't). Keep this flag on
# every Arabic font conversion below, not just the ones that visibly needed it.

# Dedicated built-in Arabic font (ArabicFontSystem's zero-setup default), bundled at
# the three sizes actually used for Arabic-eligible UI text (book titles, authors,
# filenames, chapter titles) -- SMALL_FONT_ID=8pt, UI_10_FONT_ID=10pt,
# UI_12_FONT_ID=12pt (see fontIds.h). GfxRenderer::drawArabicText/getArabicTextWidth
# pick whichever of these three matches the caller's requested fontId, so Arabic text
# renders at the same size/baseline as the surrounding Latin text instead of a single
# fixed size that overflows small rows or grid cells. --script arabic swaps
# fontconvert.py's default Latin/Cyrillic interval set for a minimal one sized for
# this font alone (basic Latin + Arabic blocks only).
ARABIC_UI_FONT_SIZES=(8 10 12)
for size in ${ARABIC_UI_FONT_SIZES[@]}; do
  python fontconvert.py notosansarabic_${size}_regular ${size} \
    ../builtinFonts/source/NotoSansArabic/NotoSansArabic-Regular.ttf \
    --2bit --compress --script arabic --reposition-marks > ../builtinFonts/notosansarabic_${size}_regular.h
  echo "Generated ../builtinFonts/notosansarabic_${size}_regular.h"
done

# Built-in Arabic READING fonts (ArabicFontSystem's default reading families) at the
# four reading sizes matching NOTOSERIF sizes -- see kBuiltinArabicReadingFontIds.
ARABIC_READING_FONT_SIZES=(12 14 16 18)
for size in ${ARABIC_READING_FONT_SIZES[@]}; do
  python fontconvert.py notonaskharabic_${size}_regular ${size} \
    ../builtinFonts/source/NotoNaskhArabic/NotoNaskhArabic-Regular.ttf \
    --2bit --compress --script arabic --reposition-marks > ../builtinFonts/notonaskharabic_${size}_regular.h
  echo "Generated ../builtinFonts/notonaskharabic_${size}_regular.h"
done
# Amiri: the Quran's own default reading face (see QuranBook::ensureExtracted /
# ArabicFontSystem's kBuiltinArabicReadingFontIds).
for size in ${ARABIC_READING_FONT_SIZES[@]}; do
  python fontconvert.py amiri_${size}_regular ${size} \
    ../builtinFonts/source/Amiri/Amiri-Regular.ttf \
    --2bit --compress --script arabic --reposition-marks > ../builtinFonts/amiri_${size}_regular.h
  echo "Generated ../builtinFonts/amiri_${size}_regular.h"
done

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
