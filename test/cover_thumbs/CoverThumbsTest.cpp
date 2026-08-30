// Host tests for the book-cover thumbnail pipeline: which file a screen asks
// for, whether the cached BMP is usable, and how it is fitted into a cover box.
//
// These exist to separate the four ways a cover can fail to appear, which all
// look identical on the panel (the same white-strip-over-black-block
// placeholder): (a) no cover art at all, (b) an invalid or stale-size cache,
// (c) an allocation failure during the blit, (d) a draw that was skipped. The
// firmware now names the case in the log (CoverDiag::Fault); these tests pin
// the classification and the pure geometry behind it, and run the REAL
// Bitmap header parser (see stubs/HalStorage.h) so top-down BMPs -- which every
// generated thumbnail is, via a negative biHeight -- stay covered.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "Bitmap.h"
#include "Logging.h"  // capturing stub; see stubs/Logging.h
#include "util/CoverDiagnostics.h"
#include "util/CoverThumbs.h"

namespace {

constexpr int kHeaderBytes = 62;  // 14 file header + 40 DIB header + 2 palette entries

void putLE16(std::string& out, const uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void putLE32(std::string& out, const uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

int strideFor(const int width) { return ((width * 1 + 31) / 32) * 4; }

// Builds a 1-bit BMP shaped exactly like the ones JpegToBmpConverter /
// PngToBmpConverter emit for cover thumbnails: palette index 0 = black,
// index 1 = white, and (when topDown) a NEGATIVE biHeight.
// `whiteAt(x, y)` is queried in image space, top row first.
template <typename Fn>
std::string makeOneBitBmp(const int width, const int height, const bool topDown, Fn whiteAt) {
  const int stride = strideFor(width);
  std::string out;
  out.reserve(kHeaderBytes + stride * height);

  out.push_back('B');
  out.push_back('M');
  putLE32(out, static_cast<uint32_t>(kHeaderBytes + stride * height));
  putLE16(out, 0);
  putLE16(out, 0);
  putLE32(out, kHeaderBytes);

  putLE32(out, 40);
  putLE32(out, static_cast<uint32_t>(width));
  putLE32(out, static_cast<uint32_t>(topDown ? -height : height));
  putLE16(out, 1);
  putLE16(out, 1);
  putLE32(out, 0);
  putLE32(out, static_cast<uint32_t>(stride * height));
  putLE32(out, 2835);
  putLE32(out, 2835);
  putLE32(out, 2);
  putLE32(out, 2);

  // Palette: 0 = black, 1 = white (BGRA).
  putLE32(out, 0x00000000);
  putLE32(out, 0x00FFFFFF);

  for (int storageRow = 0; storageRow < height; storageRow++) {
    const int imageRow = topDown ? storageRow : height - 1 - storageRow;
    std::vector<uint8_t> row(static_cast<size_t>(stride), 0);
    for (int x = 0; x < width; x++) {
      if (whiteAt(x, imageRow)) row[x >> 3] |= static_cast<uint8_t>(0x80U >> (x & 7));
    }
    out.append(reinterpret_cast<const char*>(row.data()), row.size());
  }
  return out;
}

std::string writeTempFile(const std::string& name, const std::string& bytes) {
  const std::string path = std::string(testing::TempDir()) + "/" + name;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  file.close();
  return path;
}

const uint8_t* asBytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

}  // namespace

// --- Which file does a screen ask for? -------------------------------------

TEST(CoverThumbPath, SubstitutesHeightPlaceholder) {
  EXPECT_EQ(CoverDiag::thumbPathForHeight("/.crosspoint/epub_2469617419/thumb_[HEIGHT].bmp", 300),
            "/.crosspoint/epub_2469617419/thumb_300.bmp");
}

TEST(CoverThumbPath, LeavesAResolvedPathAlone) {
  const std::string resolved = "/.crosspoint/epub_1/thumb_300.bmp";
  EXPECT_EQ(CoverDiag::thumbPathForHeight(resolved, 210), resolved);
}

TEST(CoverThumbPath, EmptyTemplateStaysEmpty) { EXPECT_EQ(CoverDiag::thumbPathForHeight("", 300), ""); }

TEST(CoverThumbPath, RecoversTheRequestedHeight) {
  EXPECT_EQ(CoverDiag::heightFromThumbPath("/.crosspoint/epub_1/thumb_300.bmp"), 300);
  EXPECT_EQ(CoverDiag::heightFromThumbPath("/.crosspoint/epub_1/thumb_210.bmp"), 210);
}

TEST(CoverThumbPath, UnresolvedTemplateHasNoHeight) {
  EXPECT_EQ(CoverDiag::heightFromThumbPath("/.crosspoint/epub_1/thumb_[HEIGHT].bmp"), 0);
  EXPECT_EQ(CoverDiag::heightFromThumbPath("/.crosspoint/epub_1/cover.bmp"), 0);
  EXPECT_EQ(CoverDiag::heightFromThumbPath(""), 0);
}

// --- Header inspection, including the top-down (negative height) convention --

TEST(BmpHeaderInspection, AcceptsATopDownOneBitThumb) {
  const std::string bmp = makeOneBitBmp(199, 300, /*topDown=*/true, [](int, int) { return true; });
  const CoverDiag::BmpHeaderInfo info = CoverDiag::inspectBmpHeader(asBytes(bmp), bmp.size());
  ASSERT_TRUE(info.ok) << info.reason;
  EXPECT_EQ(info.width, 199);
  EXPECT_EQ(info.height, 300);
  EXPECT_TRUE(info.topDown);
  EXPECT_EQ(info.bpp, 1);
  EXPECT_EQ(info.compression, 0u);
  EXPECT_EQ(info.dataOffset, static_cast<uint32_t>(kHeaderBytes));
}

TEST(BmpHeaderInspection, AcceptsABottomUpThumbWithTheSameDimensions) {
  const std::string bmp = makeOneBitBmp(199, 300, /*topDown=*/false, [](int, int) { return true; });
  const CoverDiag::BmpHeaderInfo info = CoverDiag::inspectBmpHeader(asBytes(bmp), bmp.size());
  ASSERT_TRUE(info.ok) << info.reason;
  EXPECT_EQ(info.height, 300);
  EXPECT_FALSE(info.topDown);
}

TEST(BmpHeaderInspection, RejectsTheEmptyFailureMarker) {
  // generateThumbBmp() drops a zero-byte file to record "tried and failed".
  const CoverDiag::BmpHeaderInfo info = CoverDiag::inspectBmpHeader(nullptr, 0);
  EXPECT_FALSE(info.ok);
}

TEST(BmpHeaderInspection, RejectsATruncatedFile) {
  std::string bmp = makeOneBitBmp(199, 300, true, [](int, int) { return true; });
  bmp.resize(40);
  const CoverDiag::BmpHeaderInfo info = CoverDiag::inspectBmpHeader(asBytes(bmp), bmp.size());
  EXPECT_FALSE(info.ok);
}

TEST(BmpHeaderInspection, RejectsAZeroHeightImage) {
  const std::string bmp = makeOneBitBmp(199, 0, false, [](int, int) { return true; });
  const CoverDiag::BmpHeaderInfo info = CoverDiag::inspectBmpHeader(asBytes(bmp), bmp.size());
  EXPECT_FALSE(info.ok);
}

TEST(BmpHeaderInspection, RejectsIntMinHeightBeforeNegatingIt) {
  // biHeight = 0x80000000 is INT32_MIN, which has no positive counterpart:
  // negating it to recover the top-down row count is signed-integer overflow
  // (undefined behaviour), and it happens BEFORE the "bad dimensions" check
  // that would otherwise reject the header. It has to be refused up front.
  std::string bmp = makeOneBitBmp(199, 4, /*topDown=*/true, [](int, int) { return true; });
  ASSERT_GT(bmp.size(), 26u);
  bmp[22] = 0x00;
  bmp[23] = 0x00;
  bmp[24] = 0x00;
  bmp[25] = static_cast<char>(0x80);

  const CoverDiag::BmpHeaderInfo info = CoverDiag::inspectBmpHeader(asBytes(bmp), bmp.size());
  EXPECT_FALSE(info.ok);
  EXPECT_STREQ(info.reason, "bad dimensions");
  // The negation never ran, so no bogus positive height was recorded.
  EXPECT_EQ(info.height, 0);
}

// --- The real firmware parser over the real file layout ---------------------

TEST(BitmapParser, ReadsATopDownThumbTheSameWayTheFirmwareDoes) {
  const std::string path =
      writeTempFile("cover_topdown.bmp", makeOneBitBmp(199, 300, true, [](int, int) { return true; }));

  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", path, file));
  Bitmap bitmap(file);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  EXPECT_EQ(bitmap.getWidth(), 199);
  EXPECT_EQ(bitmap.getHeight(), 300);
  EXPECT_TRUE(bitmap.isTopDown());
  EXPECT_TRUE(bitmap.is1Bit());
  EXPECT_EQ(bitmap.getRowBytes(), strideFor(199));
}

TEST(BitmapParser, MapsPaletteIndexZeroToBlackAndOneToWhite) {
  // The polarity check: if this inverted, every light cover would paint solid
  // black -- exactly the symptom this test suite was written against.
  const std::string path =
      writeTempFile("cover_polarity.bmp", makeOneBitBmp(8, 1, true, [](const int x, int) { return (x % 2) == 1; }));

  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", path, file));
  Bitmap bitmap(file);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);

  std::vector<uint8_t> out(static_cast<size_t>((bitmap.getWidth() + 3) / 4), 0);
  std::vector<uint8_t> rowBuffer(static_cast<size_t>(bitmap.getRowBytes()), 0);
  ASSERT_EQ(bitmap.readNextRow(out.data(), rowBuffer.data()), BmpReaderError::Ok);

  for (int x = 0; x < bitmap.getWidth(); x++) {
    const uint8_t val = (out[x / 4] >> (6 - ((x * 2) % 8))) & 0x3;
    if (x % 2 == 1) {
      EXPECT_EQ(val, 3) << "palette index 1 must decode to white at x=" << x;
    } else {
      EXPECT_EQ(val, 0) << "palette index 0 must decode to black at x=" << x;
    }
  }
}

TEST(CachedThumbValidity, AcceptsAGeneratedThumb) {
  const std::string path =
      writeTempFile("cover_valid.bmp", makeOneBitBmp(199, 300, true, [](int, int) { return true; }));
  EXPECT_TRUE(Bitmap::isValidCachedBmp(path));
}

TEST(CachedThumbValidity, AWrongSizedThumbIsStillWellFormed) {
  // A cache from another geometry parses fine, so the reader cannot call it
  // corrupt -- and must not, since the cover box scales any height to fit. It is
  // only classified (and logged) as a size mismatch, never thrown away: the
  // converters crop to whichever axis binds, so "not exactly the requested
  // height" is a legitimate outcome and rejecting it would regenerate forever.
  const std::string path =
      writeTempFile("cover_stale.bmp", makeOneBitBmp(139, 210, true, [](int, int) { return true; }));
  EXPECT_TRUE(Bitmap::isValidCachedBmp(path));
  EXPECT_EQ(CoverDiag::classify(true, true, true, 139, 210, 300, true, true), CoverDiag::Fault::StaleSize);
  EXPECT_TRUE(CoverDiag::fitCoverIntoBox(139, 210, 200, 300).valid);
}

TEST(CachedThumbValidity, RejectsTheEmptyFailureMarkerAndAMissingFile) {
  const std::string marker = writeTempFile("cover_marker.bmp", "");
  EXPECT_FALSE(Bitmap::isValidCachedBmp(marker));
  EXPECT_FALSE(Bitmap::isValidCachedBmp(std::string(testing::TempDir()) + "/cover_absent.bmp"));
}

// --- Fitting the thumb into its cover box ----------------------------------

TEST(CoverFit, HeroBoxUsesTheExactHeightCropPath) {
  // Foulad hero: a 199x300 thumb in the 200x300 hero box. The box is very
  // slightly wider than the art, so nothing is cropped.
  const CoverDiag::CoverFit fit = CoverDiag::fitCoverIntoBox(199, 300, 200, 300);
  ASSERT_TRUE(fit.valid);
  EXPECT_TRUE(fit.exactHeight);
  EXPECT_FLOAT_EQ(fit.cropX, 0.0f);
  EXPECT_EQ(fit.drawWidth, 199);
  EXPECT_EQ(fit.drawHeight, 300);
}

TEST(CoverFit, WiderExactHeightThumbIsCroppedToTheBox) {
  // A 204x300 thumb (the widest on a real card) in the same 200x300 box.
  const CoverDiag::CoverFit fit = CoverDiag::fitCoverIntoBox(204, 300, 200, 300);
  ASSERT_TRUE(fit.valid);
  EXPECT_TRUE(fit.exactHeight);
  EXPECT_GT(fit.cropX, 0.0f);
  EXPECT_LE(fit.drawWidth, 200);
  EXPECT_EQ(fit.drawHeight, 300);
}

TEST(CoverFit, X4ProRecentsTileShrinksTheSharedThumbToFit) {
  // X4 Pro portrait is 480 logical px wide -> 136px recents tiles.
  const CoverDiag::CoverFit fit = CoverDiag::fitCoverIntoBox(199, 300, 136, 210);
  ASSERT_TRUE(fit.valid);
  EXPECT_FALSE(fit.exactHeight);
  EXPECT_LT(fit.scale, 1.0f);
  EXPECT_LE(fit.drawWidth, 136);
  EXPECT_LE(fit.drawHeight, 210);
  EXPECT_GT(fit.drawWidth, 0);
  EXPECT_GT(fit.drawHeight, 0);
}

TEST(CoverFit, X3RecentsTileIsHeightBoundNotWidthBound) {
  // X3 portrait is 528 logical px wide -> 152px tiles, so 210/300 is the
  // binding constraint rather than the tile width.
  const CoverDiag::CoverFit fit = CoverDiag::fitCoverIntoBox(199, 300, 152, 210);
  ASSERT_TRUE(fit.valid);
  EXPECT_FLOAT_EQ(fit.scale, 210.0f / 300.0f);
  EXPECT_LE(fit.drawHeight, 210);
}

TEST(CoverFit, NeverUpscalesIntoALargerBox) {
  // drawBitmap only ever scales down; a small thumb must stay native and be
  // centered, not stretched to fill the destination rect.
  const CoverDiag::CoverFit fit = CoverDiag::fitCoverIntoBox(100, 150, 300, 400);
  ASSERT_TRUE(fit.valid);
  EXPECT_FLOAT_EQ(fit.scale, 1.0f);
  EXPECT_EQ(fit.drawWidth, 100);
  EXPECT_EQ(fit.drawHeight, 150);
  EXPECT_EQ(fit.offsetX, 100);
  EXPECT_EQ(fit.offsetY, 125);
}

TEST(CoverFit, DegenerateInputsAreRejected) {
  EXPECT_FALSE(CoverDiag::fitCoverIntoBox(0, 300, 200, 300).valid);
  EXPECT_FALSE(CoverDiag::fitCoverIntoBox(199, 300, 200, 0).valid);
  EXPECT_FALSE(CoverDiag::fitCoverIntoBox(-1, -1, -1, -1).valid);
}

// --- Telling the four failure cases apart ----------------------------------

TEST(CoverFaultClassification, DistinguishesEveryCase) {
  // (a) no cover art recorded for the book at all
  EXPECT_EQ(CoverDiag::classify(false, false, false, 0, 0, 300, true, false), CoverDiag::Fault::NoPath);
  // (a) the thumb was never generated / was removed
  EXPECT_EQ(CoverDiag::classify(true, false, false, 0, 0, 300, true, false), CoverDiag::Fault::Missing);
  // (b) a file is there but is not a well-formed BMP
  EXPECT_EQ(CoverDiag::classify(true, true, false, 0, 0, 300, true, false), CoverDiag::Fault::Invalid);
  // (b) well-formed but generated for a different geometry
  EXPECT_EQ(CoverDiag::classify(true, true, true, 139, 210, 300, true, false), CoverDiag::Fault::StaleSize);
  // (c) the blit could not allocate its buffers
  EXPECT_EQ(CoverDiag::classify(true, true, true, 199, 300, 300, false, false), CoverDiag::Fault::OutOfMemory);
  // (d) the renderer declined to paint
  EXPECT_EQ(CoverDiag::classify(true, true, true, 199, 300, 300, true, false), CoverDiag::Fault::NotPainted);
  // success
  EXPECT_EQ(CoverDiag::classify(true, true, true, 199, 300, 300, true, true), CoverDiag::Fault::None);
}

TEST(CoverFaultClassification, HeightCheckIsOptional) {
  // OPDS covers are cached at the source's native size (never upscaled), so
  // their consumers pass 0 and must not be told the cache is stale.
  EXPECT_EQ(CoverDiag::classify(true, true, true, 120, 180, 0, true, true), CoverDiag::Fault::None);
}

TEST(CoverFaultClassification, DegenerateDimensionsCountAsInvalid) {
  EXPECT_EQ(CoverDiag::classify(true, true, true, 0, 300, 300, true, true), CoverDiag::Fault::Invalid);
  EXPECT_EQ(CoverDiag::classify(true, true, true, 199, 0, 300, true, true), CoverDiag::Fault::Invalid);
}

TEST(CoverFaultClassification, EveryFaultHasAGreppableName) {
  EXPECT_STREQ(CoverDiag::faultName(CoverDiag::Fault::None), "OK");
  EXPECT_STREQ(CoverDiag::faultName(CoverDiag::Fault::NoPath), "NO-COVER-PATH");
  EXPECT_STREQ(CoverDiag::faultName(CoverDiag::Fault::Missing), "MISSING-COVER");
  EXPECT_STREQ(CoverDiag::faultName(CoverDiag::Fault::Invalid), "INVALID-CACHE");
  EXPECT_STREQ(CoverDiag::faultName(CoverDiag::Fault::StaleSize), "STALE-CACHE-SIZE");
  EXPECT_STREQ(CoverDiag::faultName(CoverDiag::Fault::OutOfMemory), "OOM");
  EXPECT_STREQ(CoverDiag::faultName(CoverDiag::Fault::NotPainted), "DRAW-SKIPPED");
}

// --- One greppable line per fault, emitted by probeThumb itself -------------
//
// CLAUDE.md requires a log before every error return, but the callers already
// print a "[COVER] ..." fault line of their own, so the requirement is really
// "exactly one" in both directions: a silent return leaves a cover failure
// undiagnosable, and a second line doubles every failure in the capture. These
// count the captured lines rather than merely asserting one exists.

namespace {

std::string absentPath() { return std::string(testing::TempDir()) + "/probe_absent.bmp"; }

}  // namespace

TEST(ProbeThumbLogging, EmptyPathLogsExactlyOneNamedLine) {
  test_log::clear();
  char detail[48] = {0};
  EXPECT_EQ(CoverThumbs::probeThumb("", 300, detail, sizeof(detail)), CoverDiag::Fault::NoPath);
  ASSERT_EQ(test_log::lines().size(), 1u);
  EXPECT_EQ(test_log::lines()[0].origin, "COVER");
  EXPECT_TRUE(test_log::onlyLineContains("NO-COVER-PATH"));
}

TEST(ProbeThumbLogging, MissingFileLogsExactlyOneNamedLine) {
  test_log::clear();
  char detail[48] = {0};
  EXPECT_EQ(CoverThumbs::probeThumb(absentPath(), 300, detail, sizeof(detail)), CoverDiag::Fault::Missing);
  ASSERT_EQ(test_log::lines().size(), 1u);
  EXPECT_TRUE(test_log::onlyLineContains("MISSING-COVER"));
  // Debug level, not error: "not generated yet" is the normal first-visit state
  // that the generation pass this gate feeds then heals.
  EXPECT_EQ(test_log::lines()[0].level, "DBG");
}

TEST(ProbeThumbLogging, CorruptFileLogsExactlyOneNamedLine) {
  const std::string path = writeTempFile("probe_corrupt.bmp", "not a bitmap at all, just text");
  test_log::clear();
  char detail[48] = {0};
  EXPECT_EQ(CoverThumbs::probeThumb(path, 300, detail, sizeof(detail)), CoverDiag::Fault::Invalid);
  ASSERT_EQ(test_log::lines().size(), 1u);
  EXPECT_TRUE(test_log::onlyLineContains("INVALID-CACHE"));
  EXPECT_EQ(test_log::lines()[0].level, "ERR");
  // The caller's copy of the reason matches what was logged.
  EXPECT_NE(test_log::lines()[0].text.find(detail), std::string::npos);
}

TEST(ProbeThumbLogging, EmptyFailureMarkerLogsExactlyOneNamedLine) {
  // generateThumbBmp() drops a zero-byte file to record "tried and failed".
  const std::string path = writeTempFile("probe_marker.bmp", "");
  test_log::clear();
  EXPECT_EQ(CoverThumbs::probeThumb(path, 300, nullptr, 0), CoverDiag::Fault::Invalid);
  ASSERT_EQ(test_log::lines().size(), 1u);
  EXPECT_TRUE(test_log::onlyLineContains("INVALID-CACHE"));
}

TEST(ProbeThumbLogging, WrongSizedThumbLogsExactlyOneNamedLine) {
  const std::string path =
      writeTempFile("probe_stale.bmp", makeOneBitBmp(139, 210, true, [](int, int) { return true; }));
  test_log::clear();
  char detail[48] = {0};
  EXPECT_EQ(CoverThumbs::probeThumb(path, 300, detail, sizeof(detail)), CoverDiag::Fault::StaleSize);
  ASSERT_EQ(test_log::lines().size(), 1u);
  EXPECT_TRUE(test_log::onlyLineContains("STALE-CACHE-SIZE"));
  EXPECT_TRUE(test_log::onlyLineContains("found 139x210"));
}

TEST(ProbeThumbLogging, AGoodThumbLogsNothing) {
  const std::string path = writeTempFile("probe_ok.bmp", makeOneBitBmp(199, 300, true, [](int, int) { return true; }));
  test_log::clear();
  char detail[48] = {0};
  EXPECT_EQ(CoverThumbs::probeThumb(path, 300, detail, sizeof(detail)), CoverDiag::Fault::None);
  EXPECT_TRUE(test_log::lines().empty());
  EXPECT_STREQ(detail, "");
}

TEST(ProbeThumbLogging, IsUsableThumbDoesNotSecondGuessTheProbeLine) {
  // The regression this guards: probeThumb() now logs, so the gate above it must
  // not log the same fault again. One fault, one line, either way in.
  const std::string stale =
      writeTempFile("gate_stale.bmp", makeOneBitBmp(139, 210, true, [](int, int) { return true; }));

  test_log::clear();
  EXPECT_TRUE(CoverThumbs::isUsableThumb(stale, 300));  // reported, but still usable
  EXPECT_EQ(test_log::lines().size(), 1u);

  test_log::clear();
  EXPECT_FALSE(CoverThumbs::isUsableThumb(absentPath(), 300));
  EXPECT_EQ(test_log::lines().size(), 1u);

  const std::string good = writeTempFile("gate_ok.bmp", makeOneBitBmp(199, 300, true, [](int, int) { return true; }));
  test_log::clear();
  EXPECT_TRUE(CoverThumbs::isUsableThumb(good, 300));
  EXPECT_TRUE(test_log::lines().empty());
}
