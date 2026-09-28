#pragma once

// Timing budgets for the WiFi association path.
//
// Deliberately free of Arduino/ESP-IDF headers so test/wifi_connect_policy can assert
// the budgets against the device measurements that set them, the same way
// test/x3_memory_budget pins the heap numbers it was derived from.
namespace wifi_connect_policy {

// --- Measured association times (X3, 75 attempts / 60 successes) -------------
// Timed from WiFi.begin() to WL_CONNECTED. The device log that produced these timed
// from ~684 ms earlier -- before a rolling-log SD append and a blocking panel repaint --
// and recorded a 7122 ms worst case and a 6778 ms median there, so the radio's own
// figures are those minus that fixed pre-begin cost.
inline constexpr unsigned long MEASURED_WORST_ASSOCIATION_MS = 6438;
inline constexpr unsigned long MEASURED_MEDIAN_ASSOCIATION_MS = 6094;

// --- Timeouts ----------------------------------------------------------------

// User-initiated connect: the user picked one specific network and is watching it, so
// wait the attempt out rather than guessing on their behalf.
inline constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;

// Auto-connect budget. Shorter than a manual connect because auto-connect may work
// through several saved networks in sequence, but it must still clear the measured
// distribution with real margin. The previous 7000 ms had none: on the very clock it
// bounded it sat *below* the 7122 ms worst case observed and only 222 ms (3%) above the
// 6778 ms median, with 71% of successes past 6500 ms. 11 of 75 logged attempts were cut
// off, on networks a manual retry then joined in 6506 ms.
inline constexpr unsigned long AUTO_CONNECTION_TIMEOUT_MS = 10000;

// Bound on the post-association SNTP wait, so a blocked NTP path cannot strand the UI.
inline constexpr unsigned long TIME_SYNC_TIMEOUT_MS = 5000;

// How long to wait for an observed STA disconnect event after asking a superseded
// attempt to stop, before escalating to a radio stop. Short: this is the gap between
// esp_wifi_disconnect() and its own event, not a network operation.
inline constexpr unsigned long CANCEL_CONFIRM_TIMEOUT_MS = 500;

}  // namespace wifi_connect_policy
