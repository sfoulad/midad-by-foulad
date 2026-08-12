#include "AppsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "MidadAppSettings.h"
#include "QuranBook.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/apps/GymActivity.h"
#include "activities/apps/StopwatchActivity.h"
#include "activities/apps/TasbihActivity.h"
#include "activities/games/GamesMenuActivity.h"
#include "activities/network/BluetoothActivity.h"
#include "components/TileCover.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/GridNav.h"

namespace {
constexpr int BOTTOM_MARGIN = 40;  // reserved for the button hints
constexpr int GUTTER = 12;
// Deliberately wide: two columns, not the three a book grid gets. The tile IS the
// label here rather than cover art, so a cell has to hold a whole app name -- at
// three across, "The Holy Quran" truncated to "The Holy Qur...".
constexpr int MIN_CELL_WIDTH = 200;
// Short and wide, like a button. Nothing pictorial goes inside these, so book-cover
// proportions would only add empty space.
constexpr float TILE_ASPECT = 0.55f;
}  // namespace

const std::vector<AppsActivity::AppEntry>& AppsActivity::entries() {
  // Files first (it gave up its own Home slot for this screen), then the apps in
  // the order they were introduced, which is also roughly how much they get used.
  // Quran carries no toggle here on purpose -- see launch() for why it is the one
  // app that cannot simply be switched on.
  // Setter lambdas save immediately (same convention as SettingInfo::DynamicToggle
  // in SettingsList.h) so this table is the one place that knows each app's flag
  // lives on MidadAppSettings, not the caller.
  static const std::vector<AppEntry> kEntries = {
      {AppId::Files, StrId::STR_FILES, nullptr, nullptr},
      {AppId::Quran, StrId::STR_QURAN, [] { return MIDAD_APP_SETTINGS.quranEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.quranEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      {AppId::Games, StrId::STR_GAMES, [] { return MIDAD_APP_SETTINGS.gamesEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.gamesEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      {AppId::Tasbih, StrId::STR_TASBIH, [] { return MIDAD_APP_SETTINGS.tasbihEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.tasbihEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      // News is deliberately absent: the server removed /opds/news (foulad-ebooks
      // PR #113, live 2026-08-05) after News-as-EPUB kept producing on-device parse
      // crashes -- News is an app-only feature now (the phone app renders native
      // article cards). A tile here would only ever reach a permanent 404. The
      // launch plumbing (AppId::News, goToNews) stays for a possible future
      // device-facing endpoint, but nothing routes to it.
      {AppId::Stopwatch, StrId::STR_STOPWATCH, [] { return MIDAD_APP_SETTINGS.stopwatchEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.stopwatchEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      {AppId::Pomodoro, StrId::STR_POMODORO, [] { return MIDAD_APP_SETTINGS.pomodoroEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.pomodoroEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      {AppId::Gym, StrId::STR_GYM, [] { return MIDAD_APP_SETTINGS.gymEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.gymEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      // A launcher like any other app entry below (BLE-R2) -- opens BluetoothActivity,
      // which now owns BLE's entire lifetime itself (correction 2: screen-scoped, no
      // persisted on/off setting left to back a getEnabled/setEnabled pair). Entering
      // the screen starts BLE; leaving it stops BLE. Nothing to auto-enable on first
      // tap the way a normal app's opt-in does.
      {AppId::MidadBle, StrId::STR_MIDAD_BLE, nullptr, nullptr},
  };
  return kEntries;
}

void AppsActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  requestUpdate();
}

bool AppsActivity::launch(const AppEntry& entry) {
  // Turn the app on if it is off. This is what makes listing every app safe: the
  // screen shows what the device can do, and pressing one is the whole opt-in --
  // no round trip through Settings to find out an app was there all along.
  if (entry.getEnabled && entry.getEnabled() == 0) {
    if (entry.id == AppId::Quran) {
      // The one app with a real cost to switching on: it extracts the embedded
      // EPUB to the SD card (seconds on a first enable) and re-checks that at
      // every boot, which is exactly why it is not on by default. Mirrors
      // SettingsActivity's own handling, including staying off when the card
      // cannot be written rather than claiming a book that is not there.
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      if (!QuranBook::ensureExtracted()) {
        LOG_ERR("APPS", "Quran extraction failed; leaving it disabled");
        return false;
      }
    }
    entry.setEnabled(1);
  }

  switch (entry.id) {
    case AppId::Files:
      activityManager.goToFileBrowser("/");
      return true;
    case AppId::Quran:
      // A real extracted EPUB, so it opens in the reader like any other book.
      activityManager.goToReader(QuranBook::PATH);
      return true;
    case AppId::Games:
      startActivityForResult(std::make_unique<GamesMenuActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      return true;
    case AppId::Tasbih:
      startActivityForResult(std::make_unique<TasbihActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      return true;
    case AppId::News:
      // Reboots in, like the Library does: News browses OPDS over WiFi and then
      // downloads an EPUB, the same stack of allocations that made
      // goToFouladEbooks() restart first. Does not return.
      silentRestartToNews();
      return true;
    case AppId::Stopwatch:
      startActivityForResult(std::make_unique<StopwatchActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      return true;
    case AppId::Pomodoro:
      startActivityForResult(
          std::make_unique<StopwatchActivity>(renderer, mappedInput, StopwatchActivity::Mode::Pomodoro),
          [](const ActivityResult&) {});
      return true;
    case AppId::Gym:
      startActivityForResult(std::make_unique<GymActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      return true;
    case AppId::MidadBle:
      // Opens the pairing screen -- BLE-R2 correction 2. Does not itself touch BLE;
      // BluetoothActivity's own onEnter()/onExit() request/release the radio
      // directly now (see BlePeripheralManager::setUserRequested()). Deliberately
      // still push-based (startActivityForResult), not replaceActivity() -- unlike
      // Home's long-press shortcut (correction 3), this path already reliably
      // clears the heap gate as measured on real hardware, since Apps itself has
      // nothing comparable to Home's retained cover buffer. Back correctly returns
      // to Apps, not Home.
      startActivityForResult(std::make_unique<BluetoothActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      return true;
  }
  return false;
}

void AppsActivity::loop() {
  const int count = static_cast<int>(entries().size());
  const int itemsPerPage = std::max(1, columns * rowsPerPage);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex >= 0 && selectorIndex < count) {
      if (!launch(entries()[selectorIndex])) requestUpdate();  // stayed here; repaint over the popup
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  buttonNavigator.onRelease({MappedInputManager::Button::Right}, [this, count] {
    selectorIndex = GridNav::moveHorizontal(selectorIndex, count, true);
    requestUpdate();
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Left}, [this, count] {
    selectorIndex = GridNav::moveHorizontal(selectorIndex, count, false);
    requestUpdate();
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this, count, itemsPerPage] {
    selectorIndex = GridNav::moveVertical(selectorIndex, count, columns, itemsPerPage, true);
    requestUpdate();
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this, count, itemsPerPage] {
    selectorIndex = GridNav::moveVertical(selectorIndex, count, columns, itemsPerPage, false);
    requestUpdate();
  });
}

void AppsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CAT_APPS));
  // Below the header, not a fixed offset: at 60 the first row of tiles drew straight
  // over the title.
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Same column-then-cell-size derivation the OPDS grid uses, so tiles fill the
  // panel edge-to-edge on both the X4 (480) and the wider X3 (528).
  columns = std::max(1, (pageWidth - GUTTER) / (MIN_CELL_WIDTH + GUTTER));
  const int tileWidth = (pageWidth - GUTTER * (columns + 1)) / columns;
  const int tileHeight = static_cast<int>(tileWidth * TILE_ASPECT);
  const int rowHeight = tileHeight + GUTTER;
  const int contentHeight = pageHeight - contentTop - BOTTOM_MARGIN;
  rowsPerPage = std::max(1, contentHeight / rowHeight);

  const int itemsPerPage = columns * rowsPerPage;
  const int pageStart = (selectorIndex / itemsPerPage) * itemsPerPage;
  const int count = static_cast<int>(entries().size());
  const int pageCount = std::min(itemsPerPage, count - pageStart);

  const int totalGridWidth = columns * (tileWidth + GUTTER) - GUTTER;
  const int gridStartX = std::max(0, (pageWidth - totalGridWidth) / 2);

  for (int i = 0; i < pageCount; i++) {
    const int idx = pageStart + i;
    const auto& entry = entries()[idx];
    const int col = i % columns;
    const int row = i / columns;
    const int cellX = gridStartX + col * (tileWidth + GUTTER);
    const int cellY = contentTop + row * rowHeight;

    // Same designed name-tile the news feeds and collection tiles use, rather
    // than eight new icons: the label IS the thing being identified here. Midad BLE
    // no longer shows an On/Off suffix (correction 2, screen-scoped BLE): with no
    // persisted setting left, "On" would be misleading almost all the time -- BLE
    // only actually runs while the pairing screen itself is open.
    const std::string tileLabel = I18N.get(entry.label);
    drawTileCover(renderer, cellX, cellY, tileWidth, tileHeight, tileLabel.c_str());
    if (idx == selectorIndex) {
      // Matches the OPDS grid's selection frame -- a 1px outline is hard to find
      // at a glance on e-ink across a multi-column grid.
      renderer.drawRect(cellX - 4, cellY - 4, tileWidth + 8, tileHeight + 8, 4, true);
    }
    // No caption underneath: drawTileCover already sets the name inside the tile, so
    // one was printed twice.
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), true);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
