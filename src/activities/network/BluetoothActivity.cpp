#include "BluetoothActivity.h"

#include <BlePeripheralManager.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "I18n.h"
#include "MappedInputManager.h"
#include "components/icons/bluetooth64.h"
#include "fontIds.h"
#include "reading/ReadingStatsStore.h"

void BluetoothActivity::onEnter() {
  Activity::onEnter();
  lastRenderedState_ = -1;

  // Aggressively release everything reclaimable that this simple a page has no use
  // for, before requesting the radio -- see this class's own header comment. Both
  // are proven-safe to call cross-activity (global singletons, not owned by
  // whichever activity happened to fill them): OtaUpdateActivity.cpp already does
  // the same pair for the same reason (needing every spare block before a memory-
  // heavy operation).
  READING_STATS.releaseMemory();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

  BlePeripheral.setUserRequested(true);
  LOG_DBG("BLE", "BluetoothActivity entered, requested BLE on (free heap=%u)",
          static_cast<unsigned>(ESP.getFreeHeap()));
  requestUpdate();
}

void BluetoothActivity::onExit() {
  // Radio teardown itself happens via main.cpp's normal bleAllowedNow lifecycle
  // (isUserRequested() flips false, the very next tick's begin()/end() branch reacts)
  // -- no reboot needed here, unlike CrossPointWebServerActivity's WiFi teardown;
  // BlePeripheralManager::end() has never left fragmentation behind all the times
  // this session tested it on and off.
  BlePeripheral.setUserRequested(false);
  Activity::onExit();
}

void BluetoothActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Redraw whenever the radio's own state has moved on since the last paint --
  // main.cpp's global state-transition nudge already calls requestUpdate() on any
  // active activity when this happens, but checking here too means this activity
  // repaints correctly even if that nudge is ever missed, and costs nothing when it
  // isn't (requestUpdate() no-ops if nothing changed record-to-record isn't true --
  // it always schedules a render, so only call it on an actual change).
  const int currentState = static_cast<int>(BlePeripheral.state());
  if (currentState != lastRenderedState_) {
    requestUpdate();
  }
}

void BluetoothActivity::render(RenderLock&&) {
  lastRenderedState_ = static_cast<int>(BlePeripheral.state());

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int iconSize = 64;
  const int iconX = (pageWidth - iconSize) / 2;
  const int iconY = pageHeight / 2 - iconSize - 20;
  renderer.drawIcon(Bluetooth64Icon, iconX, iconY, iconSize);

  const int titleY = iconY + iconSize + 24;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_MIDAD_BLE), true, EpdFontFamily::BOLD);

  const bool connected = BlePeripheral.state() == BlePeripheralManager::State::Connected;
  const char* statusText = connected ? tr(STR_CONNECTED) : tr(STR_BLUETOOTH_WAITING);
  const int statusY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 12;
  renderer.drawCenteredText(SMALL_FONT_ID, statusY, statusText, true);

  if (!connected) {
    const int hintY = statusY + renderer.getLineHeight(SMALL_FONT_ID) + 20;
    renderer.drawCenteredText(SMALL_FONT_ID, hintY, tr(STR_BLUETOOTH_HINT), true);
  }

  const int exitHintY = pageHeight - renderer.getLineHeight(SMALL_FONT_ID) - 20;
  renderer.drawCenteredText(SMALL_FONT_ID, exitHintY, tr(STR_BLUETOOTH_EXIT_HINT), true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
