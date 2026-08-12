#include "BluetoothActivity.h"

#ifndef SIMULATOR
// No simulator-side counterpart exists for this HAL component (see main.cpp's own
// guarded include of the same header) -- every access below is guarded so this whole
// translation unit still compiles and renders in the simulator with a placeholder
// state, per BLE-R2's simulator-compilability goal (unlike the stale branch's design,
// which excluded this file from the simulator build entirely).
#include <BlePeripheralManager.h>
#endif
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "I18n.h"
#include "MappedInputManager.h"
#include "MidadAppSettings.h"
#include "components/icons/bluetooth64.h"
#include "fontIds.h"
#include "reading/ReadingStatsStore.h"

BluetoothActivity::UiState BluetoothActivity::currentUiState() const {
  if (!MIDAD_APP_SETTINGS.bleEnabled) return UiState::Disabled;
#ifndef SIMULATOR
  switch (BlePeripheral.state()) {
    case BlePeripheralManager::State::Off:
      return UiState::Starting;
    case BlePeripheralManager::State::Advertising:
      return UiState::Advertising;
    case BlePeripheralManager::State::Connected:
      return UiState::Connected;
    case BlePeripheralManager::State::PausedLowMemory:
      return UiState::PausedLowMemory;
  }
  return UiState::Starting;
#else
  // Simulator placeholder: enabled always reads as "about to advertise" here, since
  // there is no real radio to poll -- purely for visually validating layout/RTL, not
  // for exercising actual BLE behavior (which the simulator can never do regardless,
  // see CLAUDE.md's simulator-testing notes).
  return UiState::Advertising;
#endif
}

void BluetoothActivity::onEnter() {
  Activity::onEnter();
  lastRenderedUiState_ = -1;

  // Aggressively release everything reclaimable that this simple a page has no use
  // for -- both proven safe to call cross-activity (global singletons, not owned by
  // whichever activity happened to fill them): OtaUpdateActivity.cpp already does the
  // same pair for the same reason (needing every spare block before a memory-heavy
  // operation). This doesn't request BLE itself -- it only maximizes the headroom
  // available whenever main.cpp's own bleAllowedNow gate next tries begin().
  READING_STATS.releaseMemory();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

  requestUpdate();
}

void BluetoothActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Same persisted setting the Apps tile already flips -- this screen is a better
    // place to show the result, not a new capability. Never touches BLE lifetime
    // directly; main.cpp's existing bleAllowedNow gate reacts on its own next tick.
    MIDAD_APP_SETTINGS.bleEnabled = MIDAD_APP_SETTINGS.bleEnabled ? 0 : 1;
    MIDAD_APP_SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  const int nowState = static_cast<int>(currentUiState());
  if (nowState != lastRenderedUiState_) {
    requestUpdate();
  }
}

void BluetoothActivity::render(RenderLock&&) {
  const UiState state = currentUiState();
  lastRenderedUiState_ = static_cast<int>(state);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int iconSize = 64;
  const int iconX = (pageWidth - iconSize) / 2;
  const int iconY = pageHeight / 2 - iconSize - 40;
  renderer.drawIcon(Bluetooth64Icon, iconX, iconY, iconSize);

  const int titleY = iconY + iconSize + 20;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_MIDAD_BLE), true, EpdFontFamily::BOLD);

  // Device name: static regardless of state (a pure eFuse read, needs no radio up) --
  // shown whenever BLE is enabled at all, so the user can cross-check it against a
  // scanner even while the radio is still starting up.
  int nextY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  if (state != UiState::Disabled) {
    char advName[24] = {};
#ifndef SIMULATOR
    BlePeripheralManager::getAdvertisedName(advName, sizeof(advName));
#else
    snprintf(advName, sizeof(advName), "Midad-XXXXXX");
#endif
    renderer.drawCenteredText(SMALL_FONT_ID, nextY, advName, true);
    nextY += renderer.getLineHeight(SMALL_FONT_ID) + 12;
  } else {
    nextY += 12;
  }

  const char* statusText = tr(STR_BLUETOOTH_STARTING);
  const char* hintText = nullptr;
  switch (state) {
    case UiState::Disabled:
      statusText = tr(STR_BLUETOOTH_OFF);
      hintText = tr(STR_BLUETOOTH_ENABLE_HINT);
      break;
    case UiState::Starting:
      statusText = tr(STR_BLUETOOTH_STARTING);
      break;
    case UiState::Advertising:
      statusText = tr(STR_BLUETOOTH_READY);
      hintText = tr(STR_BLUETOOTH_OPEN_APP_HINT);
      break;
    case UiState::Connected:
      statusText = tr(STR_CONNECTED);
      break;
    case UiState::PausedLowMemory:
      statusText = tr(STR_BLUETOOTH_PAUSED_LOW_MEMORY);
      break;
  }
  renderer.drawCenteredText(UI_12_FONT_ID, nextY, statusText, true, EpdFontFamily::BOLD);
  nextY += renderer.getLineHeight(UI_12_FONT_ID) + 8;

  if (hintText) {
    renderer.drawCenteredText(SMALL_FONT_ID, nextY, hintText, true);
  }

  // Bottom hints: exit always shown; enable/disable only when it isn't already
  // implied by the status line's own hint above (Disabled already shows the enable
  // hint there, so only add the disable hint here for the "on" states).
  const int exitHintY = pageHeight - renderer.getLineHeight(SMALL_FONT_ID) - 20;
  renderer.drawCenteredText(SMALL_FONT_ID, exitHintY, tr(STR_BLUETOOTH_EXIT_HINT), true);
  if (state != UiState::Disabled) {
    const int toggleHintY = exitHintY - renderer.getLineHeight(SMALL_FONT_ID) - 4;
    renderer.drawCenteredText(SMALL_FONT_ID, toggleHintY, tr(STR_BLUETOOTH_DISABLE_HINT), true);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
