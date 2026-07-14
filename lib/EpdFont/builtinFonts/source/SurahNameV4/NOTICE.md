## Source

`surah-name-v4.ttf` — calligraphic surah-name banner typeface, King Fahd Complex-adjacent
provenance (same Madinah Mushaf project as UthmanicHafs). Obtained from the font bundle of
[zeeyado/quran-ebook](https://github.com/zeeyado/quran-ebook) (GPL-3.0 tool; the font
itself is distributed there, sourced from [QUL](https://qul.tarteel.ai/)).

This font has no cmap entries for its calligraphy glyphs at all -- the source project
renders each surah name by shaping an ASCII trigger string (e.g. `"surah018surah-icon"`)
through the font's own GSUB rules via HarfBuzz, which resolves to exactly 2 glyphs: one
surah-specific calligraphy glyph, and one ornament glyph shared by all 114 surahs. This
repo's renderer doesn't run GSUB, so `lib/EpdFont/scripts/fontconvert.py --glyph-map`
precomputes each surah's shaped-glyph pair once at build time (via `uharfbuzz`) and bakes
each one as a direct bitmap under a dedicated PUA codepoint -- see
`tools/quran/gen_surah_banner_glyphmap.py` and `GfxRenderer::parseSurahBannerMarker`.

## License

King Fahd Complex: use, copy, and distribute permitted; modification not permitted.

Converting this font into this project's compressed embedded bitmap format is arguably a
modification. This was a deliberate, informed decision — not an oversight — matching the
same decision already made for UthmanicHafs (see its own NOTICE.md).
