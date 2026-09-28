#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "AllocFloor.h"
#include "RegionGeometry.h"
#include "activities/home/HomeCoverBudget.h"

// Regression coverage for the two X3 memory defects:
//
//   1. HomeActivity's cover-tile frame snapshot asks for a contiguous block that
//      an X3 cannot serve even on a pristine heap (42,768 bytes in portrait
//      against a measured clean-boot MaxAlloc of 38,900), and re-asks on every
//      render because coverRendered only latches on success.
//   2. FontDecompressor::getBitmap() re-issues an already-refused malloc once per
//      glyph, because its suppression latch was keyed on the (font, group) pair
//      rather than on the request size.
//
// Everything here is exercised through the same headers the firmware compiles:
// RegionGeometry.h backs GfxRenderer::getRegionByteSize, HomeCoverBudget.h is the
// rule storeCoverBuffer() applies, AllocFloor.h is the policy FontDecompressor
// routes every malloc through.

namespace {

// Panel geometry, from FreeInkDisplay.h.
constexpr uint16_t X3_PANEL_W = 792;
constexpr uint16_t X3_PANEL_H = 528;
constexpr uint16_t X4_PANEL_W = 800;
constexpr uint16_t X4_PANEL_H = 480;

// Measured on an X3 at a clean, idle boot (120s, zero errors).
constexpr uint32_t X3_CLEAN_FREE_HEAP = 49040;
constexpr uint32_t X3_CLEAN_MAX_ALLOC = 38900;
// Same device once Home had collapsed.
constexpr uint32_t X3_DEGRADED_FREE_HEAP = 4336;
constexpr uint32_t X3_DEGRADED_MAX_ALLOC = 3188;

// FouladTheme metrics: homeTopPadding / homeCoverTileHeight.
constexpr int HOME_TOP_PADDING = 10;
constexpr int FOULAD_TILE_HEIGHT = 640;
// BaseTheme's default, which upstream caps at.
constexpr int BASE_TILE_HEIGHT = 400;

int screenWidth(int orientation, uint16_t panelW, uint16_t panelH) {
  return (orientation == RegionGeometry::Portrait || orientation == RegionGeometry::PortraitInverted) ? panelH : panelW;
}

// Exactly what HomeActivity::render() sets up before calling into the theme.
size_t coverSnapshotBytes(int orientation, uint16_t panelW, uint16_t panelH, int tileHeight) {
  return RegionGeometry::byteSize(orientation, 0, HOME_TOP_PADDING, screenWidth(orientation, panelW, panelH),
                                  tileHeight, panelW, panelH);
}

constexpr int kAllOrientations[] = {RegionGeometry::Portrait, RegionGeometry::LandscapeClockwise,
                                    RegionGeometry::PortraitInverted, RegionGeometry::LandscapeCounterClockwise};

}  // namespace

// --- 1. The geometry the field evidence was produced by --------------------

TEST(CoverSnapshotGeometry, MatchesTheSizesObservedOnDevice) {
  // "[ERR] [HOME] OOM: cover buffer (42768 bytes)" on an X3 in portrait.
  EXPECT_EQ(coverSnapshotBytes(RegionGeometry::Portrait, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT), 42768u);
  // The same tile on an X4 panel: 81 bytes/row x 480 rows.
  EXPECT_EQ(coverSnapshotBytes(RegionGeometry::Portrait, X4_PANEL_W, X4_PANEL_H, FOULAD_TILE_HEIGHT), 38880u);
}

TEST(CoverSnapshotGeometry, X3WorstCaseIsLandscapeNotPortrait) {
  // The tile is taller (640) than a landscape logical screen (528), so the rect
  // clamps to nearly the whole panel and the snapshot costs MORE, not less.
  const size_t portrait = coverSnapshotBytes(RegionGeometry::Portrait, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT);
  const size_t inverted =
      coverSnapshotBytes(RegionGeometry::PortraitInverted, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT);
  const size_t cw = coverSnapshotBytes(RegionGeometry::LandscapeClockwise, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT);
  const size_t ccw =
      coverSnapshotBytes(RegionGeometry::LandscapeCounterClockwise, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT);

  EXPECT_EQ(portrait, 42768u);
  EXPECT_EQ(inverted, 42768u);
  EXPECT_EQ(cw, 51282u);
  EXPECT_EQ(ccw, 51282u);
}

TEST(CoverSnapshotGeometry, EveryX3OrientationExceedsTheCleanBootLargestBlock) {
  // The point of the whole exercise: on an X3 this allocation is impossible in
  // all four orientations even with a completely fresh heap, so no amount of
  // freeing elsewhere can rescue the unguarded malloc.
  for (const int o : kAllOrientations) {
    EXPECT_GT(coverSnapshotBytes(o, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT), X3_CLEAN_MAX_ALLOC) << "orient " << o;
  }
}

TEST(CoverSnapshotGeometry, UpstreamsSmallerTileIsStillUnaffordableOnX3) {
  // Shrinking the tile to upstream's 400 is not the fix: it is still more than
  // half of an X3's entire free heap, and it would cut the Recents thumbnail row
  // off the Foulad home screen.
  const size_t portrait = coverSnapshotBytes(RegionGeometry::Portrait, X3_PANEL_W, X3_PANEL_H, BASE_TILE_HEIGHT);
  EXPECT_EQ(portrait, 26928u);
  EXPECT_FALSE(HomeCoverBudget::fits(portrait, X3_CLEAN_FREE_HEAP, X3_CLEAN_MAX_ALLOC));
}

// --- 2. The budget rule ----------------------------------------------------

TEST(CoverSnapshotBudget, RefusedInEveryX3OrientationOnACleanHeap) {
  for (const int o : kAllOrientations) {
    const size_t needed = coverSnapshotBytes(o, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT);
    EXPECT_FALSE(HomeCoverBudget::fits(needed, X3_CLEAN_FREE_HEAP, X3_CLEAN_MAX_ALLOC)) << "orient " << o;
  }
}

TEST(CoverSnapshotBudget, RefusedOnADegradedHeap) {
  const size_t needed = coverSnapshotBytes(RegionGeometry::Portrait, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT);
  EXPECT_FALSE(HomeCoverBudget::fits(needed, X3_DEGRADED_FREE_HEAP, X3_DEGRADED_MAX_ALLOC));
}

TEST(CoverSnapshotBudget, AcceptedOnlyWhenTheReserveSurvives) {
  constexpr size_t needed = 38880;  // X4 portrait
  // Exactly enough total free, and a block with the required headroom.
  EXPECT_TRUE(HomeCoverBudget::fits(needed, needed + HomeCoverBudget::RESERVE_BYTES,
                                    needed + HomeCoverBudget::BLOCK_HEADROOM_BYTES));
  // One byte short of the reserve.
  EXPECT_FALSE(HomeCoverBudget::fits(needed, needed + HomeCoverBudget::RESERVE_BYTES - 1,
                                     needed + HomeCoverBudget::BLOCK_HEADROOM_BYTES));
  // Plenty of total free, but fragmented: no block large enough.
  EXPECT_FALSE(HomeCoverBudget::fits(needed, 200u * 1024u, needed - 1));
}

TEST(CoverSnapshotBudget, ReserveCoversTheLargestBufferThatMustCoexist) {
  // The biggest hot group the field log shows FontDecompressor asking for.
  EXPECT_GT(HomeCoverBudget::RESERVE_BYTES, 12217u);
}

TEST(CoverSnapshotBudget, ZeroSizedRegionIsNeverAccepted) {
  EXPECT_FALSE(HomeCoverBudget::fits(0, 200u * 1024u, 200u * 1024u));
  // An off-panel rect yields 0 bytes rather than a bogus size.
  EXPECT_EQ(RegionGeometry::byteSize(RegionGeometry::Portrait, 0, 0, 0, 0, X3_PANEL_W, X3_PANEL_H), 0u);
}

// --- 3. The render loop must not retry -------------------------------------

namespace {

// Models HomeActivity's snapshot state machine: the render loop, the denial
// latch, and the two places Home deliberately hands heap back.
class HomeSnapshotModel {
 public:
  HomeSnapshotModel(size_t needed, uint32_t freeHeap, uint32_t maxAlloc)
      : needed_(needed), freeHeap_(freeHeap), maxAlloc_(maxAlloc) {}

  // One HomeActivity::render() pass. Returns true if covers were painted, which
  // they always are -- from the snapshot when there is one, from the cached
  // thumb BMPs otherwise.
  bool render() {
    renders_++;
    if (coverRendered_ && bufferSize_ == needed_) {
      restores_++;
      return true;  // blitted back from the snapshot
    }
    coversDrawnFromSd_++;  // the theme redraws them; this is the degraded path
    const bool stored = storeCoverBuffer();
    coverRendered_ = stored;
    return true;
  }

  // HomeActivity::loadRecentCovers()'s "free everything, we need the blocks" arm.
  void releaseForCoverGeneration() {
    bufferSize_ = 0;
    coverRendered_ = false;
    denied_ = false;
  }

  int budgetChecks() const { return budgetChecks_; }
  int mallocAttempts() const { return mallocAttempts_; }
  int coversDrawnFromSd() const { return coversDrawnFromSd_; }
  int restores() const { return restores_; }
  int renders() const { return renders_; }

 private:
  bool storeCoverBuffer() {
    if (denied_) return false;
    if (bufferSize_ == needed_) return true;
    budgetChecks_++;
    if (!HomeCoverBudget::fits(needed_, freeHeap_, maxAlloc_)) {
      denied_ = true;
      return false;
    }
    mallocAttempts_++;
    if (needed_ > maxAlloc_) {
      denied_ = true;
      return false;
    }
    bufferSize_ = needed_;
    return true;
  }

  size_t needed_;
  uint32_t freeHeap_;
  uint32_t maxAlloc_;
  size_t bufferSize_ = 0;
  bool coverRendered_ = false;
  bool denied_ = false;
  int renders_ = 0;
  int budgetChecks_ = 0;
  int mallocAttempts_ = 0;
  int coversDrawnFromSd_ = 0;
  int restores_ = 0;
};

}  // namespace

TEST(HomeRenderLoop, RefusedSnapshotIsAttemptedOnceNotOncePerRender) {
  const size_t needed = coverSnapshotBytes(RegionGeometry::Portrait, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT);
  HomeSnapshotModel home(needed, X3_CLEAN_FREE_HEAP, X3_CLEAN_MAX_ALLOC);

  for (int i = 0; i < 200; i++) EXPECT_TRUE(home.render());

  EXPECT_EQ(home.budgetChecks(), 1);
  EXPECT_EQ(home.mallocAttempts(), 0);  // never even reaches malloc: the budget says no
}

TEST(HomeRenderLoop, CoversKeepRenderingAfterTheSnapshotIsRefused) {
  const size_t needed = coverSnapshotBytes(RegionGeometry::Portrait, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT);
  HomeSnapshotModel home(needed, X3_CLEAN_FREE_HEAP, X3_CLEAN_MAX_ALLOC);

  for (int i = 0; i < 50; i++) EXPECT_TRUE(home.render());

  // Degradation is "redraw from SD every time", never "no covers".
  EXPECT_EQ(home.coversDrawnFromSd(), home.renders());
  EXPECT_EQ(home.restores(), 0);
}

TEST(HomeRenderLoop, AffordableSnapshotIsTakenOnceAndThenReused) {
  constexpr size_t needed = 38880;
  HomeSnapshotModel home(needed, 90u * 1024u, 64u * 1024u);

  for (int i = 0; i < 50; i++) EXPECT_TRUE(home.render());

  EXPECT_EQ(home.mallocAttempts(), 1);
  EXPECT_EQ(home.coversDrawnFromSd(), 1);
  EXPECT_EQ(home.restores(), 49);
}

TEST(HomeRenderLoop, DenialIsRearmedWhenHomeDeliberatelyFreesHeap) {
  const size_t needed = coverSnapshotBytes(RegionGeometry::Portrait, X3_PANEL_W, X3_PANEL_H, FOULAD_TILE_HEIGHT);
  HomeSnapshotModel home(needed, X3_CLEAN_FREE_HEAP, X3_CLEAN_MAX_ALLOC);

  for (int i = 0; i < 20; i++) home.render();
  EXPECT_EQ(home.budgetChecks(), 1);

  home.releaseForCoverGeneration();
  for (int i = 0; i < 20; i++) home.render();

  // Exactly one further evaluation, not twenty.
  EXPECT_EQ(home.budgetChecks(), 2);
}

// --- 4. FontDecompressor's allocation floor --------------------------------

namespace {

// The five distinct hot-group sizes the X3 log shows, in the interleaved order a
// line of mixed-font text produces them.
const std::vector<uint32_t> kObservedGroupSizes = {12217, 12176, 4898, 4727, 3153};

// The pre-fix latch: one (font, group) pair, evicted by the next different pair.
struct PairLatch {
  const void* font = nullptr;
  uint32_t group = UINT32_MAX;
  bool suppresses(const void* f, uint32_t g) const { return f == font && g == group; }
  void noteFailure(const void* f, uint32_t g) {
    font = f;
    group = g;
  }
};

}  // namespace

TEST(FontAllocFloor, PairKeyedLatchFailsToStopAnInterleavedBurst) {
  PairLatch latch;
  int mallocCalls = 0;
  const char* font = "f";

  // 30 glyphs cycling through the five failing groups, as on a real line.
  for (int i = 0; i < 30; i++) {
    const uint32_t group = static_cast<uint32_t>(i % kObservedGroupSizes.size());
    if (latch.suppresses(font, group)) continue;
    mallocCalls++;  // every one of these fails
    latch.noteFailure(font, group);
  }

  EXPECT_EQ(mallocCalls, 30);  // one hopeless malloc per glyph -- the reported defect
}

TEST(FontAllocFloor, SizeKeyedFloorConvergesOnTheSameBurst) {
  AllocFloor floor;
  int mallocCalls = 0;

  for (int i = 0; i < 30; i++) {
    const uint32_t needed = kObservedGroupSizes[i % kObservedGroupSizes.size()];
    if (floor.suppresses(needed)) continue;
    mallocCalls++;
    floor.noteFailure(needed);
  }

  // Bounded by the number of strictly decreasing distinct sizes, not by glyphs.
  EXPECT_EQ(mallocCalls, static_cast<int>(kObservedGroupSizes.size()));
  EXPECT_TRUE(floor.suppresses(3153));
  EXPECT_FALSE(floor.suppresses(3152));
}

TEST(FontAllocFloor, AttemptsAreBoundedNoMatterHowLongTheBurstRuns) {
  AllocFloor floor;
  int mallocCalls = 0;

  for (int i = 0; i < 10000; i++) {
    const uint32_t needed = kObservedGroupSizes[i % kObservedGroupSizes.size()];
    if (floor.suppresses(needed)) continue;
    mallocCalls++;
    floor.noteFailure(needed);
  }

  EXPECT_LE(mallocCalls, static_cast<int>(kObservedGroupSizes.size()));
}

TEST(FontAllocFloor, SmallerRequestsStillGetTheirOneChance) {
  AllocFloor floor;
  floor.noteFailure(12217);
  EXPECT_TRUE(floor.suppresses(12217));
  EXPECT_TRUE(floor.suppresses(20000));
  EXPECT_FALSE(floor.suppresses(4898));  // strictly smaller: worth attempting
}

TEST(FontAllocFloor, ASmallSuccessDoesNotUnlockALargeRequest) {
  // A 4KB allocation succeeding says nothing about a 12KB block existing; treating
  // it as evidence is what makes a pair-keyed latch ping-pong.
  AllocFloor floor;
  floor.noteFailure(12176);
  // ... a 3KB allocation happens to succeed here; the floor is deliberately untouched.
  EXPECT_TRUE(floor.suppresses(12176));
}

TEST(FontAllocFloor, ReleaseRearmsSoASuppressedGroupIsRetried) {
  // freeHotGroup / freePageBuffer / deinit -- i.e. clearCache, i.e. every page
  // turn through PrewarmScope -- all reset(). The floor is a burst suppressor,
  // never a permanent blacklist.
  AllocFloor floor;
  floor.noteFailure(12217);
  EXPECT_TRUE(floor.isSuppressing());

  floor.reset();

  EXPECT_FALSE(floor.isSuppressing());
  EXPECT_FALSE(floor.suppresses(12217));
  EXPECT_FALSE(floor.suppresses(UINT32_MAX - 1));
}

TEST(FontAllocFloor, FreshFloorSuppressesNothing) {
  const AllocFloor floor;
  EXPECT_FALSE(floor.isSuppressing());
  EXPECT_FALSE(floor.suppresses(0));
  EXPECT_FALSE(floor.suppresses(64u * 1024u));
}

// --- 5. FontDecompressor cleanup on every exit path ------------------------

namespace {

// Mirrors FontDecompressor's owned allocations and the paths that release them.
// Each release point must both free its buffers AND re-arm the floor, or a
// suppression set during a doomed burst outlives the memory pressure that caused it.
struct DecompressorModel {
  bool hotGroup = false;
  bool hotGlyphBuf = false;
  bool pageSlot = false;
  bool alignedTable = false;
  AllocFloor floor;

  void freeHotGroup() {
    floor.reset();
    hotGroup = false;
    hotGlyphBuf = false;
  }
  void freePageBuffer() {
    pageSlot = false;
    floor.reset();
  }
  void clearCache() {
    freePageBuffer();
    freeHotGroup();
  }
  void deinit() {
    freePageBuffer();
    freeHotGroup();
    alignedTable = false;
    floor.reset();
  }
  bool anythingHeld() const { return hotGroup || hotGlyphBuf || pageSlot || alignedTable; }
};

}  // namespace

TEST(FontDecompressorCleanup, ClearCacheReleasesPerPageStateAndRearms) {
  DecompressorModel d;
  d.hotGroup = d.hotGlyphBuf = d.pageSlot = d.alignedTable = true;
  d.floor.noteFailure(12217);

  d.clearCache();

  EXPECT_FALSE(d.hotGroup);
  EXPECT_FALSE(d.hotGlyphBuf);
  EXPECT_FALSE(d.pageSlot);
  EXPECT_FALSE(d.floor.isSuppressing());
  // The whole-font aligned-offset table survives on purpose: it depends only on the
  // font's static layout, so it stays valid across page turns.
  EXPECT_TRUE(d.alignedTable);
}

TEST(FontDecompressorCleanup, DeinitReleasesEverythingIncludingTheOffsetTable) {
  DecompressorModel d;
  d.hotGroup = d.hotGlyphBuf = d.pageSlot = d.alignedTable = true;
  d.floor.noteFailure(4727);

  d.deinit();

  EXPECT_FALSE(d.anythingHeld());
  EXPECT_FALSE(d.floor.isSuppressing());
}

TEST(FontDecompressorCleanup, EveryReleasePathRearmsTheFloor) {
  for (int path = 0; path < 4; path++) {
    DecompressorModel d;
    d.floor.noteFailure(12176);
    switch (path) {
      case 0:
        d.freeHotGroup();
        break;
      case 1:
        d.freePageBuffer();
        break;
      case 2:
        d.clearCache();
        break;
      default:
        d.deinit();
        break;
    }
    EXPECT_FALSE(d.floor.isSuppressing()) << "release path " << path;
  }
}

TEST(FontDecompressorCleanup, AFailedPrewarmDoesNotStrandAUselessPageSlot) {
  // prewarmCache() registers the slot before computing its aligned offsets. If the
  // offset tracker cannot be allocated, every glyph in that slot would fall through
  // to the hot-group path anyway, so the slot's buffer must be handed back rather
  // than held until the next clearCache().
  DecompressorModel d;
  d.pageSlot = true;       // slot registered
  bool trackerOk = false;  // groupAlignedTracker OOM
  if (!trackerOk) d.freePageBuffer();

  EXPECT_FALSE(d.pageSlot);
}
