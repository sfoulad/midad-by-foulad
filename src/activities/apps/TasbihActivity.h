#pragma once

#include "activities/Activity.h"

// Simple dhikr (tasbih) counter, opened from the "Tasbih" tile in My Books
// (see TASBIH_PSEUDO_PATH in RecentBooksActivity.cpp). Either side button
// (Up or Down -- the physical, non-remappable buttons, so it always works
// regardless of the user's front-button remap or orientation) increments
// today's count, shown big and centered; the footer shows the all-time best
// single day ("Top Tasbih") and the running total for the current calendar
// year, both from TasbihStore.
class TasbihActivity final : public Activity {
 public:
  explicit TasbihActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Tasbih", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Set when a tap just landed on a standard dhikr milestone (33/99/100) --
  // render() draws the count inverted (white-on-black) for that one pass as a
  // small "you hit it" cue, then clears the flag so the next redraw is back to
  // normal. No timer/auto-revert: e-ink has no capacity for a real timed blink
  // without wasting a whole extra full-refresh cycle just to undo it, so the
  // flash simply lasts until the next natural redraw (next button press).
  bool milestoneFlash = false;
};
