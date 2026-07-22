#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// One row of the downloaded exercise catalog (see GYM_STORE_TASKS.md's
// GET /api/gym/catalog contract). Metadata only -- no instructions text (that
// and the image are fetched lazily, per-exercise, only once the user adds it
// to a day; see GymDownloadActivity, Phase 2).
struct GymCatalogEntry {
  std::string slug;
  std::string name;
  std::string bodyPart;
  std::string equipment;
  uint32_t imageSize = 0;
  uint32_t imageCrc32 = 0;
  uint16_t imageWidth = 0;
  uint16_t imageHeight = 0;
};

// One distinct body part present in the catalog, with how many exercises it
// has -- see GymCatalog::loadBodyPartCounts().
struct GymBodyPartCount {
  std::string bodyPart;
  int count = 0;
};

namespace GymCatalog {
// Locally-cached copy of the server catalog, written once by
// GymDownloadActivity after a successful fetch; read directly (no re-download)
// by the exercise browser every time it opens.
constexpr const char* CATALOG_PATH = "/gym/catalog.json";
// Safety cap on parse -- comfortably above the ~500-800 exercises the source
// dataset actually has (see GYM_STORE_TASKS.md PART 0); guards against a
// corrupt/hostile catalog file forcing an unbounded allocation.
constexpr size_t MAX_CATALOG_ENTRIES = 2000;

// Validates that the file at `path` parses as a well-formed, version-1
// catalog and reports the exercise count via `countOut`, WITHOUT retaining a
// single entry in memory. Use this to validate a file while WiFi/TLS may
// still be connected -- GymCatalogSyncActivity validates the
// freshly-downloaded temp file before WiFi has torn down (see its onExit()),
// and heap pressure is highest right then.
bool validate(const std::string& path, size_t& countOut);

// Distinct body parts present in the catalog with per-body-part exercise
// counts, WITHOUT retaining any exercise entries -- memory is bounded to the
// small, fixed number of distinct body parts in the dataset (~17), not the
// ~873 exercises. This is deliberately the ONLY way the exercise browser
// looks at the whole catalog at once: an earlier version streamed all ~873
// entries into one std::deque<GymCatalogEntry>, and even deque's own
// internal chunk-map growth threw bad_alloc against the real device's
// fragmented heap (confirmed via device crash log; std::vector failed the
// same way earlier still). Loading only a per-body-part subset (see
// loadExercisesForBodyPart below) keeps every allocation small.
bool loadBodyPartCounts(std::vector<GymBodyPartCount>& out);

// Only the exercises whose bodyPart == `bodyPart`, into `out` (cleared
// first). Bounds memory to a single body part's subset (worst case ~148 for
// Quadriceps) instead of the full ~873-entry catalog -- see
// loadBodyPartCounts's comment for why the full catalog is never loaded at
// once.
bool loadExercisesForBodyPart(const std::string& bodyPart, std::deque<GymCatalogEntry>& out);

// Local path an exercise's image is (or would be) downloaded to.
std::string imagePath(const std::string& slug);
// Local path an exercise's instructions text is (or would be) cached to.
std::string instructionsPath(const std::string& slug);

// Local path a pre-dithered, 1-bit BMP thumbnail of the exercise's photo is
// (or would be) cached to -- see ensureImageThumb(). Dimensions are baked
// into the filename (like book cover thumbs -- see UITheme::getCoverThumbPath)
// so bumping the on-screen image size in a future firmware update generates a
// fresh thumb instead of reusing a stale, wrong-size one left on the SD card
// by an older version.
std::string imageThumbPath(const std::string& slug, int maxWidth, int maxHeight);
// Generates the 1-bit BMP thumbnail for `slug` from its downloaded JPEG if not
// already cached (validated the same way book cover thumbs are -- see
// Bitmap::isValidCachedBmp -- so a partial file from an interrupted write
// regenerates instead of sticking forever). Uses
// JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize, the same converter
// HomeActivity/RecentBooksActivity use for cover art: a real 1-bit
// error-diffusion dither (Atkinson) targeting BW output directly, unlike the
// JPEG-decoder's own 4-level dither which only looks right when pushed
// through the reader's multi-pass grayscale panel sequence -- rendering a
// standalone photo through that sequence collapses most tones to "no
// signal" (confirmed on-device: image showed as a faint gray blob instead of
// a photo). Returns false (and logs) on decode/write failure; caller should
// treat the exercise as imageless in that case.
bool ensureImageThumb(const std::string& slug, int maxWidth, int maxHeight);
}  // namespace GymCatalog
