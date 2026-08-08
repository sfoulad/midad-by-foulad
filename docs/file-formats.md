# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8.

## `book.bin`

### Version 9

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

Version 9 ignores ambiguous EPUB 2 `<guide>` `type="text"` references when
locating the book's first-reading position: many EPUB 2 files mark every
content file as `"text"`, so that type alone doesn't reliably identify a
start location. Only an explicit `type="start"` reference is now used;
otherwise the reader opens at spine index 0.

Version 8 (undocumented at the time) stores TOC/book titles NFC-composed.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 9
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

### Version 39

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 39 adds the book-internal source href to each serialized ImageBlock,
written right after the cache path (lazy image extraction: section builds only
header-probe images for dimensions, and the file is extracted from the EPUB on
the page's first render).

Version 38 is a pure cache-bust (no structural change from v37) for two
upstream layout fixes (#2652, #2679): CJK `MAX_WORD_SIZE` continuation words no
longer gain a false leading space, and bare text after a closing block tag no
longer inherits the closed block's style.

Version 37 adds an optional `kashidaExtraPx[wordCount]` array to TextBlock's
arena (kashida/tatweel justification for Arabic body text): the extra width,
already floored to a whole tatweel-glyph multiple, that a word should absorb
via kashida when rendered. Omitted from the arena entirely when no word on
the line got a kashida share (non-Arabic paragraphs, or lines whose spare
space went entirely to inter-word gaps) -- zero per-word RAM cost when
inactive, same convention as the `focusSuffixX`/`focusBoundary` pair. Changes
the arena's byte layout and size, so v36 cached blocks can't be read as v37.

Version 36 changed how ayah-number marker words
(`"\xEF\xB4\xBF<digits>\xEF\xB4\xBE"`) lay out: their cached x-position is now
based on the U+06DD rosette glyph's advance instead of their per-glyph text
width, matching how the renderer actually draws them (as a medallion) --
cached x-positions from v35 no longer match.

Version 35 (this fork) is upstream's v29 format -- flat TextBlock word storage
and the incremental-build INCOMPLETE/PARTIAL version sentinels -- plus this
fork's extra header field: the resolved Arabic reading font id, written right
after `fontId` and compared on load, so changing the Arabic Font Size (or an
SD Arabic font override) re-paginates like any other layout setting. The fork
numbers formats independently of upstream: v29-v34 below were fork versions.

Version 34 added the Arabic reading font id to the header cache key (see
above; first shipped in fork v1.6.59 with upstream's v28 body format).

Version 33 is a pure cache-bust (no structural change from v32): the gap between
two Arabic words is now sized by the Arabic font's own space glyph (which follows
the `arabicFontSize` setting) instead of the Latin reading font's space. When
Arabic is set larger than the Latin reading size, the old Latin-sized gap was too
small for the big Arabic glyphs and words ran together; this changes inter-word x
positions on Arabic lines without touching any cache-key settings field.

Version 32 is a pure cache-bust (no structural change from v31), covering two
Arabic fixes that change layout: (a) `<li><p>text</p></li>` no longer strands
the bullet marker on its own line -- a block tag nested directly inside a
fresh `<li>` continues in the bullet's block; (b) mid-word alef maksura shapes
with the yeh initial/medial presentation forms (the dotless FBE8/FBE9 forms
aren't in the bundled fonts, and mid-word ى in real text is the Egyptian yeh
convention), which changes measured word widths and therefore line breaks.

Version 31 is a pure cache-bust (no structural change from v30), covering two
Arabic/RTL layout fixes: (a) CSS `text-align: start`/`end` now resolve against
the paragraph's direction (an RTL paragraph with `text-align: start` is
right-aligned; previously both values were statically folded to left/right at
CSS parse time), and (b) Arabic words are no longer fallback-hyphenated
mid-word (Arabic doesn't hyphenate, and the split broke cursive joining).
Neither touched a cache-busting settings field, so it needs an explicit
version bump. The CSS compile cache version (`CssParser::CSS_CACHE_VERSION`)
was bumped 7 -> 8 in the same change, since cached compiled stylesheets built
before it hold the wrong (physical) values for `start`/`end` declarations.

Version 30 is a pure cache-bust (no structural change from v29): it forces
sections cached under the earlier, Latin-only per-line row pitch to rebuild
after upgrading. `ChapterHtmlSlimParser::addLineToPage` now sizes a line's row
height using the taller of the Latin reading font and the Arabic font when the
line contains Arabic text, instead of always using the Latin font's line
height alone -- the old pitch could clip Arabic glyph ascenders/descenders.
None of this touched a cache-busting settings field, so it needs an explicit
version bump.

Version 29 is a pure cache-bust (no structural change from v28): it forces
sections cached under earlier, incorrect Arabic layout logic (word width
measurement, word-order reordering, tashkeel/ligature/digit-range shaping) to
rebuild after upgrading, since none of those fixes touched a cache-busting
settings field.

Version 35 includes:

- cache-busting fields for paragraph alignment, hyphenation, embedded CSS,
  image rendering mode, and Focus Reading
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- optional per-word Focus Reading split metadata
- per-page footnote entries
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage (v29): per-word arrays plus one shared
  NUL-terminated text blob, replacing v28's length-prefixed word strings. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 35
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageHorizontalRule = 3
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
};

struct TextBlock {
    u16 wordCount;
    u8 hasFocus;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasFocus != 0) {
            u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasFocus != 0) {
            u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    s32 arabicFontId [[comment("Fork addition: resolved Arabic reading font id")]];
    float lineCompression;
    bool extraParagraphSpacing;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool focusReadingEnabled;

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```
