#include <gtest/gtest.h>

#include "activities/settings/OtaUpdateScreenModel.h"

using ota_screen::Actions;
using ota_screen::actionsFor;
using ota_screen::Screen;

// Every screen either firmware-update surface can show must expose a valid
// action -- on button hardware AND on touch hardware, where button hints never
// render (BaseTheme::drawButtonHints returns immediately when gpio.hasTouch()).
// The X4 Pro defect was exactly this: the Library offer screen's only controls
// were button hints, so it rendered with no Update or Cancel control at all.
TEST(OtaScreenActions, EveryScreenIsOperableOnButtonsAndTouch) {
  for (uint8_t i = 0; i < ota_screen::SCREEN_COUNT; i++) {
    const Actions actions = actionsFor(static_cast<Screen>(i));
    EXPECT_TRUE(ota_screen::operable(actions)) << "screen " << static_cast<int>(i);
    EXPECT_TRUE(ota_screen::touchOperable(actions)) << "screen " << static_cast<int>(i);
  }
}

// The two offer/confirmation screens answer through the Cancel/Update popup,
// which provides touch targets and button navigation in one component -- and
// never auto-start anything.
TEST(OtaScreenActions, OfferScreensUseTheConfirmPopup) {
  for (const Screen screen : {Screen::CONFIRMING, Screen::LIBRARY_OFFER}) {
    const Actions actions = actionsFor(screen);
    EXPECT_TRUE(actions.confirmPopup);
    EXPECT_TRUE(actions.acceptBack);  // declining must always be possible
    EXPECT_FALSE(actions.busy);
  }
}

// Terminal informational screens exit on Back (buttons) or any tap (touch).
TEST(OtaScreenActions, NoUpdateAndFailedExitOnBackOrTap) {
  for (const Screen screen : {Screen::NO_UPDATE, Screen::FAILED}) {
    const Actions actions = actionsFor(screen);
    EXPECT_TRUE(actions.acceptBack);
    EXPECT_TRUE(actions.acceptTap);
    EXPECT_FALSE(actions.busy);
  }
}

// In-flight and terminal-transition screens expect no input; they advance on
// their own (and the activities hold off auto-sleep while they do).
TEST(OtaScreenActions, BusyScreensExpectNoInput) {
  for (const Screen screen : {Screen::CHECKING, Screen::INSTALLING, Screen::FINISHED, Screen::SHUTTING_DOWN}) {
    const Actions actions = actionsFor(screen);
    EXPECT_TRUE(actions.busy);
    EXPECT_FALSE(actions.confirmPopup);
    EXPECT_FALSE(actions.acceptBack);
    EXPECT_FALSE(actions.acceptTap);
  }
}

// WiFi selection delegates to its own sub-activity.
TEST(OtaScreenActions, WifiSelectionIsDelegated) { EXPECT_TRUE(actionsFor(Screen::WIFI_SELECTION).delegated); }
