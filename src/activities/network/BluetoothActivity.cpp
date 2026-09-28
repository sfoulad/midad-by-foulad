#include "BluetoothActivity.h"

#include "SilentRestart.h"

#ifndef SIMULATOR
// No simulator-side counterpart exists for this HAL component (see main.cpp's own
// guarded include of the same header) -- every access below is guarded so this whole
// translation unit still compiles and renders in the simulator with a placeholder
// state, per BLE-R2's simulator-compilability goal (unlike the stale branch's design,
// which excluded this file from the simulator build entirely).
#include <BlePeripheralManager.h>

#include "BleWifiAutoConnectCache.h"
#include "BleWifiScanCache.h"
#endif
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include "I18n.h"
#include "MappedInputManager.h"
#include "components/icons/bluetooth64.h"
#include "fontIds.h"
#include "reading/ReadingStatsStore.h"

#ifndef SIMULATOR
// Combined WiFi line for the two BLE-driven radio state machines. Nullptr when
// neither is doing anything, so the screen stays clean in the normal case.
const char* BluetoothActivity::wifiStatusText() {
  switch (BleWifiScanCache::currentState()) {
    case BleWifiScanCache::State::PendingStart:
    case BleWifiScanCache::State::Scanning:
      return tr(STR_BLUETOOTH_WIFI_SCANNING);
    case BleWifiScanCache::State::Failed:
      return tr(STR_BLUETOOTH_WIFI_FAILED);
    default:
      break;
  }
  switch (BleWifiAutoConnectCache::currentState()) {
    case BleWifiAutoConnectCache::State::PendingStart:
    case BleWifiAutoConnectCache::State::Connecting:
      return tr(STR_BLUETOOTH_WIFI_CONNECTING);
    case BleWifiAutoConnectCache::State::Done:
      return tr(STR_BLUETOOTH_WIFI_CONNECTED);
    case BleWifiAutoConnectCache::State::Failed:
      return tr(STR_BLUETOOTH_WIFI_FAILED);
    default:
      break;
  }

  // Both machines are reset to Idle by consume(), which the dispatcher calls the
  // moment the PHONE reads the result -- so a Done/Failed shown only from those
  // states can vanish before the user has looked up. Fall back to what is
  // actually true of the radio, which nothing else can clear.
  if (WiFi.status() == WL_CONNECTED) return tr(STR_BLUETOOTH_WIFI_CONNECTED);
  return nullptr;
}

// One value covering both machines, so loop() can notice a change and repaint.
int BluetoothActivity::wifiStateKey() {
  return static_cast<int>(BleWifiScanCache::currentState()) * 16 +
         static_cast<int>(BleWifiAutoConnectCache::currentState());
}
#endif

BluetoothActivity::UiState BluetoothActivity::currentUiState() const {
#ifndef SIMULATOR
  switch (BlePeripheral.state()) {
    case BlePeripheralManager::State::Off:
      // Off with the request already released means BleWifiAutoConnectCache handed
      // the radio to WiFi on a successful connect -- not a radio still coming up.
      return BlePeripheral.isUserRequested() ? UiState::Starting : UiState::HandedToWifi;
    case BlePeripheralManager::State::Advertising:
      return UiState::Advertising;
    case BlePeripheralManager::State::Connected:
      return UiState::Connected;
    case BlePeripheralManager::State::PausedLowMemory:
      return UiState::PausedLowMemory;
  }
  return UiState::Starting;
#else
  // Simulator placeholder: always reads as "about to advertise" here, since there is
  // no real radio to poll -- purely for visually validating layout/RTL, not for
  // exercising actual BLE behavior (which the simulator can never do regardless, see
  // CLAUDE.md's simulator-testing notes).
  return UiState::Advertising;
#endif
}

void BluetoothActivity::onEnter() {
  Activity::onEnter();
  lastRenderedUiState_ = -1;

  // Aggressively release everything reclaimable that this simple a page has no use
  // for, before requesting the radio -- both proven safe to call cross-activity
  // (global singletons, not owned by whichever activity happened to fill them):
  // OtaUpdateActivity.cpp already does the same pair for the same reason (needing
  // every spare block before a memory-heavy operation).
  READING_STATS.releaseMemory();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

#ifndef SIMULATOR
  // BLE and WiFi are mutually exclusive here: main.cpp's bleAllowedNow gate requires
  // WiFi.getMode() == WIFI_MODE_NULL, so a radio left in STA mode by an earlier flow
  // means begin() is never called and this screen sits on "Starting Bluetooth..."
  // forever. Nothing logged it either, because the gate only reports on a TRANSITION
  // and it never transitioned.
  //
  // Reproduced on the X3 by pairing, signing out and re-pairing (all of which leave
  // WiFi up) and then opening this screen. Neither entry point -- the Apps tile nor
  // Home's hold-Confirm shortcut -- powered the radio down, and only the Settings
  // Wi-Fi screen ever did.
  //
  // This activity owns BLE's lifetime, so it owns the precondition too. Saved
  // credentials auto-reconnect whenever a later flow needs WiFi.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);  // credentials are kept: eraseap defaults false
    WiFi.mode(WIFI_OFF);
    poweredWifiDown_ = true;
    LOG_INF("BLE", "WiFi powered off so the radio can start");
  }

  BlePeripheral.setUserRequested(true);
#endif

  requestUpdate();
}

void BluetoothActivity::onExit() {
#ifndef SIMULATOR
  // Mirror of onEnter()'s request -- this is the ONLY place BLE gets released, so
  // every exit path (Back, or navigating elsewhere) must go through here. Radio
  // teardown itself happens via main.cpp's normal bleAllowedNow lifecycle on the
  // next tick, same as it always has.
  BlePeripheral.setUserRequested(false);

  // Leave via a restart rather than bringing WiFi back up in place.
  //
  // Restoring it here was right about the symptom and expensive about the cure:
  // without it the next screen that wanted the network looked broken ("wifi didn't
  // connect" after using BLE). But WiFi.begin() costs ~30KB and the heap never gets
  // the contiguity back -- measured on the X3 leaving this screen, free 83,304
  // before the restore and 53,480 after, which puts Home under the 42,768 bytes its
  // cover snapshot needs and makes every repaint 1,478 ms instead of ~15. Reported
  // from the device as "device very very slow".
  //
  // A restart answers both: the heap comes back pristine, and WiFi lands in its
  // cold-boot state -- the state every network screen is known to work from.
  if (poweredWifiDown_) {
    poweredWifiDown_ = false;

    // BLE must be torn down SYNCHRONOUSLY first. setUserRequested(false) above only
    // takes effect when main.cpp's bleAllowedNow gate re-evaluates on its NEXT loop
    // iteration, which a restart never reaches -- leaving NimBLE's ~57KB resident
    // across the reboot. end() is a full synchronous teardown, a no-op when off.
    if (BlePeripheral.isActive()) {
      BlePeripheral.end();
    }

    Activity::onExit();
    LOG_INF("BLE", "Leaving Bluetooth; restarting for a clean heap");
    if (returnToApps_) {
      silentRestartToApps();  // does not return
    } else {
      silentRestart();  // does not return
    }
  }
#endif
  Activity::onExit();
}

void BluetoothActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

#ifndef SIMULATOR
  // WiFi -> BLE, the other half of the handover. After a phone-initiated connect
  // the radio belongs to WiFi and BLE is off deliberately, so there was no way
  // back without leaving and re-entering this screen -- and the phone's own
  // message ("Hold Confirm on it, then try again") asks for exactly this.
  //
  // Powering WiFi down here rather than only setting the request is what makes it
  // immediate: main.cpp's gate refuses BLE while WiFi is up, so a request alone
  // would sit unsatisfied with nothing explaining why.
  if (currentUiState() == UiState::HandedToWifi && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    poweredWifiDown_ = true;  // onExit() restores it, as on first entry
    BlePeripheral.setUserRequested(true);
    LOG_INF("BLE", "Bluetooth restarted by user; WiFi released");
    requestUpdate();
    return;
  }
#endif

#ifndef SIMULATOR
  // The radio machines advance on the main loop independently of BLE's own state,
  // so nothing else would repaint this screen while a scan or connect runs.
  // Latch a failure the moment it is observable: consume() can clear it before
  // the next repaint, and "it silently did nothing" is the worst thing this
  // screen can say to someone whose Wi-Fi did not come up.
  if (BleWifiAutoConnectCache::currentState() == BleWifiAutoConnectCache::State::Failed ||
      BleWifiScanCache::currentState() == BleWifiScanCache::State::Failed) {
    sawWifiFailure_ = true;
  } else if (BleWifiAutoConnectCache::currentState() == BleWifiAutoConnectCache::State::Connecting ||
             BleWifiScanCache::currentState() == BleWifiScanCache::State::Scanning) {
    sawWifiFailure_ = false;  // a new attempt supersedes the old verdict
  }

  const int wifiKey = wifiStateKey();
  if (wifiKey != lastRenderedWifiState_) {
    lastRenderedWifiState_ = wifiKey;
    requestUpdate();
  }
#endif

  const int nowState = static_cast<int>(currentUiState());
  if (nowState != lastRenderedUiState_) {
    requestUpdate();
  }
}

void BluetoothActivity::render(RenderLock&&) {
  const UiState state = currentUiState();
  lastRenderedUiState_ = static_cast<int>(state);
#ifndef SIMULATOR
  lastRenderedWifiState_ = wifiStateKey();
#endif

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
  // shown from the moment this screen opens, so the user can cross-check it against
  // a scanner even while the radio is still starting up.
  int nextY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  char advName[24] = {};
#ifndef SIMULATOR
  BlePeripheralManager::getAdvertisedName(advName, sizeof(advName));
#else
  snprintf(advName, sizeof(advName), "Midad-XXXXXX");
#endif
  renderer.drawCenteredText(SMALL_FONT_ID, nextY, advName, true);
  nextY += renderer.getLineHeight(SMALL_FONT_ID) + 12;

  const char* statusText = tr(STR_BLUETOOTH_STARTING);
  const char* hintText = nullptr;
  const char* secondHintText = nullptr;
  switch (state) {
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
    case UiState::HandedToWifi:
      statusText = tr(STR_BLUETOOTH_HANDED_TO_WIFI);
      // The phone cannot browse files over WiFi alone: the web server only runs
      // while File Transfer is open here. Say so, rather than leaving the user on
      // a screen that reports success while the app times out.
      hintText = tr(STR_BLUETOOTH_FILE_TRANSFER_HINT);
      secondHintText = tr(STR_BLUETOOTH_RESTART_HINT);
      break;
  }
  // Wrapped, not centred-and-clipped: these are sentences now, not one-word
  // states. "Bluetooth stopped so Wi-Fi can stay connected" ran off both edges of
  // the panel when drawn unwrapped, which is the same way the sign-out
  // confirmation used to fail.
  nextY +=
      renderer.drawCenteredTextWrapped(UI_12_FONT_ID, nextY, pageWidth - 40, statusText, 2, true, EpdFontFamily::BOLD);
  nextY += 8;

#ifndef SIMULATOR
  // What the PHONE just asked this device to do. Without it the reader shows only
  // its own BLE state, so a wifi.scan or wifi.autoconnect issued from the app is
  // invisible here -- the screen sits on "Ready" while the radio is actually busy,
  // and a failed connect looks identical to nothing having happened.
  //
  // It also explains the handover: a successful connect releases BLE (see
  // BleWifiAutoConnectCache::finishConnect), so this line is what tells the user
  // the Bluetooth link ended because WiFi took the radio, rather than because
  // something broke.
  const char* wifiText = wifiStatusText();
  if (!wifiText && sawWifiFailure_) wifiText = tr(STR_BLUETOOTH_WIFI_FAILED);
  if (wifiText) {
    renderer.drawCenteredText(SMALL_FONT_ID, nextY, wifiText, true);
    nextY += renderer.getLineHeight(SMALL_FONT_ID) + 8;
  }
#endif

  if (hintText) {
    const int maxWidth = pageWidth - 40;
    nextY += renderer.drawCenteredTextWrapped(SMALL_FONT_ID, nextY, maxWidth, hintText, 2);
  }
  if (secondHintText) {
    nextY += 6;
    renderer.drawCenteredText(SMALL_FONT_ID, nextY, secondHintText, true);
  }

  // Back is the only way out now -- and the only way BLE ever stops -- so it's the
  // only hint left at the bottom (see this class's own header comment).
  const int exitHintY = pageHeight - renderer.getLineHeight(SMALL_FONT_ID) - 20;
  renderer.drawCenteredText(SMALL_FONT_ID, exitHintY, tr(STR_BLUETOOTH_EXIT_HINT), true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
