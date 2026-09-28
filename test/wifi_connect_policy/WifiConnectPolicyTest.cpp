#include <gtest/gtest.h>

#include <cstdint>

#include "network/WifiConnectPolicy.h"

// Regression coverage for the WiFi connect-timing defect:
//
//   1. The auto-connect budget was 7000 ms while the operation it bounds had a measured
//      median of 6778 ms and a worst case of 7122 ms on the same clock -- a timeout
//      inside its own distribution, so 11 of 75 logged attempts were cut off on networks
//      that a manual retry then joined in 6506 ms.
//   2. The clock started ~684 ms before WiFi.begin(), behind a rolling-log SD append and
//      a blocking panel repaint, so display and storage time was charged to the radio.
//
// The measurements are pinned here, next to the budgets derived from them, so a future
// edit to either has to face the evidence -- same intent as test/x3_memory_budget.
//
// A BSSID/channel hint was tried here as a fast path and removed on device evidence: in
// three logged reconnects on an X3 it never once associated, including twice where the
// hint held the very BSSID and channel the following full scan then joined (channel 7,
// then channel 11). Each failure cost a flat ~3.5 s before the scan could start. Do not
// reintroduce a pinned WiFi.begin() without new hardware evidence that it associates.

namespace {

using namespace wifi_connect_policy;

// The value that shipped before this test existed, kept as an explicit negative case.
constexpr unsigned long OLD_AUTO_CONNECTION_TIMEOUT_MS = 7000;
// Raw device figures, on the old clock basis (which began before ~684 ms of SD + repaint).
constexpr unsigned long LOGGED_WORST_ELAPSED_MS = 7122;
constexpr unsigned long LOGGED_MEDIAN_ELAPSED_MS = 6778;
constexpr unsigned long LOGGED_PRE_BEGIN_OVERHEAD_MS = 684;

TEST(WifiConnectPolicy, MeasurementsMatchTheDeviceLogOnTheWifiBeginBasis) {
  EXPECT_EQ(MEASURED_WORST_ASSOCIATION_MS, LOGGED_WORST_ELAPSED_MS - LOGGED_PRE_BEGIN_OVERHEAD_MS);
  EXPECT_EQ(MEASURED_MEDIAN_ASSOCIATION_MS, LOGGED_MEDIAN_ELAPSED_MS - LOGGED_PRE_BEGIN_OVERHEAD_MS);
}

TEST(WifiConnectPolicy, AutoBudgetClearsTheMeasuredWorstCaseWithHeadroom) {
  EXPECT_GT(AUTO_CONNECTION_TIMEOUT_MS, MEASURED_WORST_ASSOCIATION_MS);
  // At least 50% margin over the worst success actually observed: 60 samples cannot
  // have found the true tail, and a marginal-signal association is slower still.
  EXPECT_GE(AUTO_CONNECTION_TIMEOUT_MS, MEASURED_WORST_ASSOCIATION_MS + MEASURED_WORST_ASSOCIATION_MS / 2);
}

TEST(WifiConnectPolicy, RejectsTheBudgetThatCausedTheFalseTimeouts) {
  // The regression itself, stated against the clock 7000 ms was actually applied to:
  // it fell below the worst success observed, and cleared the median by 3%.
  EXPECT_LT(OLD_AUTO_CONNECTION_TIMEOUT_MS, LOGGED_WORST_ELAPSED_MS);
  EXPECT_LT(OLD_AUTO_CONNECTION_TIMEOUT_MS - LOGGED_MEDIAN_ELAPSED_MS, LOGGED_MEDIAN_ELAPSED_MS / 20);

  EXPECT_GT(AUTO_CONNECTION_TIMEOUT_MS, OLD_AUTO_CONNECTION_TIMEOUT_MS);
  // Has to survive the raw (pre-fix) numbers too, so the fix does not rest on the clock
  // move alone being correct.
  EXPECT_GT(AUTO_CONNECTION_TIMEOUT_MS, LOGGED_WORST_ELAPSED_MS);
}

TEST(WifiConnectPolicy, AutoBudgetStaysShorterThanAManualConnect) {
  // Auto-connect may work through several saved networks, so it must still fail faster
  // than a user-initiated attempt the user is watching.
  EXPECT_LT(AUTO_CONNECTION_TIMEOUT_MS, CONNECTION_TIMEOUT_MS);
}

TEST(WifiConnectPolicy, CancellationConfirmWaitIsShortEnoughToBeInvisible) {
  // This bounds the gap between esp_wifi_disconnect() and its own STA disconnect event,
  // not a network operation, so it must stay far below the association budget it
  // interrupts -- otherwise the safety wait would itself become a timing regression.
  EXPECT_GT(CANCEL_CONFIRM_TIMEOUT_MS, 0UL);
  EXPECT_LE(CANCEL_CONFIRM_TIMEOUT_MS, AUTO_CONNECTION_TIMEOUT_MS / 10);
}

TEST(WifiConnectPolicy, ASupersededAttemptStillFitsInsideAManualConnect) {
  // A superseded attempt costs its own budget plus the bounded wait for the teardown to
  // be confirmed before anything replaces it. That has to stay inside the manual budget,
  // or the confirmation would turn a failed auto-connect into a visible stall.
  EXPECT_LE(AUTO_CONNECTION_TIMEOUT_MS + CANCEL_CONFIRM_TIMEOUT_MS, CONNECTION_TIMEOUT_MS);
}

TEST(WifiConnectPolicy, ClockSyncWaitIsBounded) {
  // Matches HalClock::quickSyncSystemTime's own 50 x 100 ms ceiling.
  EXPECT_EQ(TIME_SYNC_TIMEOUT_MS, 5000UL);
}

}  // namespace
