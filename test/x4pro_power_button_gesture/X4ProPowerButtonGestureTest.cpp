#include <gtest/gtest.h>

#include "X4ProPowerButtonGesture.h"

using Event = X4ProPowerButtonGesture::Event;
constexpr unsigned long kWindow = X4ProPowerButtonGesture::DOUBLE_CLICK_WINDOW_MS;

TEST(X4ProPowerButtonGesture, SingleClickDispatchesAfterWindowExpires) {
  X4ProPowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(1000, /*shortReleaseThisFrame=*/true), Event::None);
  // Idle frames while the window is still open: nothing fires yet.
  EXPECT_EQ(gesture.update(1100, false), Event::None);
  EXPECT_EQ(gesture.update(1499, false), Event::None);
  // Window closes at exactly 1000 + kWindow; one tick past that, it fires.
  EXPECT_EQ(gesture.update(1000 + kWindow + 1, false), Event::ShortClick);
  // Fires exactly once -- a later idle frame reports nothing new.
  EXPECT_EQ(gesture.update(2000, false), Event::None);
}

TEST(X4ProPowerButtonGesture, SecondClickWithinWindowTogglesFrontlightInsteadOfDispatching) {
  X4ProPowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(1000, true), Event::None);
  EXPECT_EQ(gesture.update(1200, true), Event::FrontlightToggle);
  // The consumed first click must never separately fire ShortClick later.
  EXPECT_EQ(gesture.update(2000, false), Event::None);
}

TEST(X4ProPowerButtonGesture, SecondClickExactlyAtWindowBoundaryStillCounts) {
  X4ProPowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(1000, true), Event::None);
  // <= kWindow counts as "within the window" per the implementation's boundary.
  EXPECT_EQ(gesture.update(1000 + kWindow, true), Event::FrontlightToggle);
}

TEST(X4ProPowerButtonGesture, ClickAfterWindowExpiryStartsAFreshPendingClick) {
  X4ProPowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(1000, true), Event::None);
  // Nothing arrives before expiry; the first click dispatches on its own.
  EXPECT_EQ(gesture.update(1000 + kWindow + 1, false), Event::ShortClick);
  // A later, unrelated click starts its own fresh window rather than being
  // treated as a (stale) second click of the already-dispatched first one.
  EXPECT_EQ(gesture.update(5000, true), Event::None);
  EXPECT_EQ(gesture.update(5000 + kWindow + 1, false), Event::ShortClick);
}

TEST(X4ProPowerButtonGesture, ThirdClickAfterAFrontlightToggleStartsFresh) {
  X4ProPowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(1000, true), Event::None);
  EXPECT_EQ(gesture.update(1100, true), Event::FrontlightToggle);
  // A third click right after a completed double-click pair is itself a new
  // single click, not a phantom continuation of the consumed pair.
  EXPECT_EQ(gesture.update(1150, true), Event::None);
  EXPECT_EQ(gesture.update(1150 + kWindow + 1, false), Event::ShortClick);
}

TEST(X4ProPowerButtonGesture, DelayedPollingDispatchesExpiredClickInsteadOfDroppingIt) {
  X4ProPowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(1000, true), Event::None);
  // No idle frame observed the window expiring before this next release
  // arrives (e.g. input polling was delayed) -- the first click must still
  // dispatch, not be silently discarded by arming straight onto the new one.
  EXPECT_EQ(gesture.update(1000 + kWindow + 1, true), Event::ShortClick);
  // This release becomes the new pending click; a follow-up shortly after
  // still completes a double-click toggle against it.
  EXPECT_EQ(gesture.update(1000 + kWindow + 1 + 100, true), Event::FrontlightToggle);
}

TEST(X4ProPowerButtonGesture, NoClicksEverProducesNoEvents) {
  X4ProPowerButtonGesture gesture;
  for (unsigned long t = 0; t < 5000; t += 10) {
    EXPECT_EQ(gesture.update(t, false), Event::None);
  }
}

// A long-press power-button hold is a different gesture entirely (handled by
// the caller before this state machine is ever consulted): the caller must
// never pass shortReleaseThisFrame=true for it, so this state machine sees
// only idle frames and correctly reports nothing for the whole hold.
TEST(X4ProPowerButtonGesture, LongPressNeverReportedAsAClick) {
  X4ProPowerButtonGesture gesture;
  for (unsigned long t = 0; t < 2000; t += 50) {
    EXPECT_EQ(gesture.update(t, false), Event::None);
  }
}
