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

### Version 43

Version 43 keeps the version 42 serialized layout unchanged. It was bumped
because parse *output* changed, not the format: unclosed HTML void elements
(`<br>`, `<hr>`, `<img>`, ...) are now closed before expat sees them, so a
chapter that previously failed to parse — and cached that failure — now lays
out correctly. Without the bump those caches would never be rebuilt, and the
fix could not reach any book already opened on the device, which is the case
it exists for.

### Version 45

Version 45 keeps the version 44 serialized layout unchanged. It was bumped
because internal EPUB links now preserve CSS superscript and subscript styles,
changing their cached word-style flags and page layout.

### Version 44
### Version 42

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 44 appends the internal-link rectangles produced during text layout to
each serialized page. The reader uses these rectangles for touch navigation;
older caches are rebuilt because they contain no link geometry.

Version 43 keeps the version 42 serialized layout unchanged. It was bumped
because paragraph base direction now excludes direction changes from inline
elements.

Version 42 keeps the version 41 serialized layout unchanged. It was bumped
because closing a block now strips inherited vertical margins and padding.

Version 41 keeps the version 40 serialized layout unchanged. It was bumped
because simple HTML table rows are now laid out as positioned columns rather
than flattened paragraphs with synthetic row/cell labels.
Version 42 keeps the version 41 serialized layout unchanged. It was bumped for
two upstream fixes landing together: ruby groups no longer split across a
soft text-block flush (#3102), and simple HTML table rows now lay out as
positioned columns instead of flattened paragraphs with synthetic row/cell
labels (#2654) -- this fork's earlier basic table support (#980, #372) is
superseded by the column layout. Also folds in an upstream image-decode-
overflow/viewport-clipping fix (#2959) that clamps image top margins so a
full-height image cannot overflow the page bottom.

Every version note in this changelog describes a delta; the ImHex pattern
below (`EXPECTED_VERSION 42`) is the full cumulative current layout. Fields
added by earlier versions -- e.g. `ImageBlock.srcPath` (v39) and `TextBlock`
ruby support (v40) -- are serialized in every `sections/*.bin` file at the
current version; they are not optional or a separate format.

Version 41 is an upstream merge (#2892). Focus Reading line breaking changed:
a visible hyphen/dash inside a word is now a break opportunity, and
hyphenation of a focus-split word considers the whole word instead of only
its regular-weight suffix. Pages cached by older versions were laid out with
the previous, more restrictive break set and no longer match.

Version 40 is an upstream merge. TextBlock gained `<ruby>`/`<rt>` support:
rubies are serialized per-block alongside the existing `kashidaExtraPx` array,
so a line can carry both Arabic kashida justification and CJK ruby annotations
at once. Word-gap suppression is now scoped to tokens glued together in the
source only, so ruby element boundaries no longer collapse the spaces between
Hangul words (previously handled by dropping the gap at any CJK break
opportunity). The header now reads its fields from a `ReaderRenderSpec`
parameter instead of bare `Section` members -- no on-disk layout change, but
`arabicFontId` is derived from `spec.fontId` rather than a stored member. A
fifth placeholder `uint32_t` (visible-offset LUT) joins the header, and each
page persists a `uint32_t` visible-text start offset. `FootnoteEntry::href`
grew from 96 to 256 bytes, changing each serialized footnote record from 128
to 288 bytes. All of the above invalidates every cache from v39 and earlier.

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
- per-page visible-text offset LUT (zero-based Unicode codepoints in `<body>`)
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs retained for navigation and legacy sync fallback
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

#define EXPECTED_VERSION 42
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 256

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
    String srcPath;
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
    u32 visibleTextLutOffset;

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

    if (visibleTextLutOffset != 0) {
	u32 visibleTextOffset[pageCount] @ visibleTextLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## CLX1 — library index (`.crosspoint/library.idx`)

Written by `lib/LibraryIndex/LibraryBuilder.cpp`, read by `LibraryIndexFile`. One
file describing every book on the card, so the shelf can sort and search
thousands of titles without opening any of them.

Format version 2. An index written by another version fails validation on open
and is rebuilt; that is the entire migration mechanism.

### Layout

| Section | Offset | Contents |
|---|---|---|
| Header | 0 | 64 bytes, `ClixHeader` |
| Folders | `folderStart` | length-prefixed paths, one per folder |
| Records | `recordStart` | `bookCount` × 128-byte `ClixRecord` |
| Permutations | `permStart` | `bookCount` u16 author order, then `bookCount` u16 arrival order |
| Name blob | `nameStart` | per record: path hash, name, canonical author, title, source author (see below) |

The arrival permutation runs oldest first, keyed by the record's FAT
modification time (when the file landed on the card); `firstSeen` — the
build-assigned discovery counter — breaks ties and carries books whose
filesystem reports no time. Fold version 3 introduced the timestamp key; a
fold bump rebuilds ranks while preserving `firstSeen`.

Sections are 512-byte aligned so each starts on an SD block boundary.

### Records are exactly 128 bytes

A fixed stride is what lets the reader seek straight to record *n* without an
offset table, and read a screenful in one 4 KB block. `static_assert` enforces it.

Each record carries `fold[96]`, the title normalised for search and sorting —
accents stripped, case dropped, leading articles removed — and `authorKey[12]`,
the author's words folded and sorted so that "Victor Hugo" and "Hugo Victor" group as
one person. `authorKey` is a GROUPING key, not an ordering one: the shelf orders by
surname, derived separately from the display name.

The byte before the folded title records metadata extraction status: not
attempted, extracted, or failed. The final four bytes contain the packed FAT
modification date and time returned by SdFat. A zero timestamp is not trusted.
These fields occupy the alignment and reserved bytes from version 1, so the
record remains exactly 128 bytes.

The header records whether EPUB metadata extraction was enabled for the build.
This prevents a metadata-disabled rebuild from making filename fallbacks look
fresh to a later metadata-enabled build.

### The name blob

Per record, at `nameStart + nameOff`:

```text
[u64 pathHash]    FNV-1a fingerprint of the complete path
[nameLen bytes]  filename, without the directory
[u8][author]     display author, one spelling chosen per authorKey across the library
[u8][title]      the book's own title, or length 0 if it never gave one
[u8][source]     cleaned author spelling before the library-wide spelling vote
```

The filename must stay the first textual field and stay the filename: `readPath`
rebuilds a book's path from it, so writing the display title there makes the book
impossible to open. That was a real defect, and it is why title has its own field.

The source author is separate from the displayed canonical author so a later
rebuild can repeat the spelling vote after books are added or removed. Existing
display reads still stop at the author or title fields and retain their offsets.

### Freshness and unchanged rebuilds

Reconciliation treats the persisted 64-bit complete-path fingerprint as the
book identity. Metadata is reused only when the fingerprint, size, nonzero FAT
timestamp, fold version, metadata mode, and expected extraction status agree.
EPUBs with a zero timestamp or a previous extraction failure are parsed again.

If every current record reuses metadata, the old and new counts agree, and no
unreadable entry was seen, the staging files are discarded and the live index is
left byte-for-byte unchanged. A normal rebuild action is therefore a freshness
check, not a forced metadata reread.

### Header flags

`RANKS_DEGRADED` says one or more orders fell back to walk order because a
checked sort allocation failed. Title and author each use a phase-local
`SortKey[bookCount]` allocation (14 bytes per book, 57,344 bytes at the 4,096-book
format ceiling); the first array is released before the second is requested.
Sorting is therefore best effort through the full format limit rather than
being disabled at an arbitrary library size.

`DEDUP_DEGRADED` says a directory exceeded the fixed 1024-entry duplicate-key
buffer, or that its fallible 8 KiB allocation failed. The walk still indexes
every enumerated book; it only stops remembering additional identities for
duplicate-dirent detection, so a damaged FAT may expose duplicates but cannot
make a real book disappear.

`selfSize` is the expected file size. Comparing it against the real one is a free
truncation guard: a build cut short by a power failure cannot pass.
