#include "AppsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "AppsGridLayout.h"
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
#include "components/UITheme.h"
#include "components/icons/appLauncherIcons.h"
#include "components/icons/folder.h"
#include "fontIds.h"
#include "util/GridNav.h"

int AppsActivity::rememberedSelectorIndex = 0;

namespace {
constexpr int BOTTOM_MARGIN = 40;  // reserved for the button hints
constexpr int GUTTER = 12;
constexpr int ICON_SIZE = 32;
constexpr int ICON_LABEL_GAP = 6;
// Selection ring: drawn OUTSIDE the tile's own thin border, same "just outside
// the cell" idiom the OPDS/My Books grids use for their 4px frame -- but here
// composed of a thick outer stroke plus a thinner nested one (the "thick double
// border" ask), a solid leading-edge indicator bar, and a focus arrowhead.
constexpr int RING_MARGIN = 6;
constexpr int RING_OUTER_WIDTH = 3;
constexpr int RING_INNER_INSET = 5;
constexpr int INDICATOR_WIDTH = 6;
constexpr int ARROW_HALF_HEIGHT = 5;
constexpr int PAGINATION_HEIGHT = 24;
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
      {AppId::Files, StrId::STR_FILES, FolderIcon, nullptr, nullptr},
      {AppId::Quran, StrId::STR_QURAN, QuranIcon, [] { return MIDAD_APP_SETTINGS.quranEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.quranEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      {AppId::Games, StrId::STR_GAMES, GamesIcon, [] { return MIDAD_APP_SETTINGS.gamesEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.gamesEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      {AppId::Tasbih, StrId::STR_TASBIH, TasbihIcon, [] { return MIDAD_APP_SETTINGS.tasbihEnabled; },
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
      {AppId::Stopwatch, StrId::STR_STOPWATCH, StopwatchIcon, [] { return MIDAD_APP_SETTINGS.stopwatchEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.stopwatchEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      {AppId::Pomodoro, StrId::STR_POMODORO, PomodoroIcon, [] { return MIDAD_APP_SETTINGS.pomodoroEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.pomodoroEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      {AppId::Gym, StrId::STR_GYM, GymIcon, [] { return MIDAD_APP_SETTINGS.gymEnabled; },
       [](uint8_t v) {
         MIDAD_APP_SETTINGS.gymEnabled = v;
         MIDAD_APP_SETTINGS.saveToFile();
       }},
      // A launcher like any other app entry below (BLE-R2) -- opens BluetoothActivity,
      // which now owns BLE's entire lifetime itself (correction 2: screen-scoped, no
      // persisted on/off setting left to back a getEnabled/setEnabled pair). Entering
      // the screen starts BLE; leaving it stops BLE. Nothing to auto-enable on first
      // tap the way a normal app's opt-in does.
      {AppId::MidadBle, StrId::STR_MIDAD_BLE, MidadBleIcon, nullptr, nullptr},
  };
  return kEntries;
}

void AppsActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = AppsGridLayout::clampSelection(rememberedSelectorIndex, static_cast<int>(entries().size()));
  requestUpdate();
}

void AppsActivity::onExit() {
  rememberedSelectorIndex = selectorIndex;
  Activity::onExit();
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

  // Whole-tile touch target: a tap anywhere on a tile launches it directly (the
  // standard app-grid tap idiom), rather than requiring a select-then-confirm
  // second tap that nothing else in this codebase does for touch. Same
  // AppsActivity, same FouladTheme, same registry on every board -- touch
  // boards (X4 Pro today) get a larger effective hit target by extending each
  // tile's hit zone halfway into its surrounding gutter (see
  // AppsGridLayout::resolveGridLane); button-only boards never call
  // wasScreenTapped() successfully (no touch hardware to report one), so this
  // whole block is a no-op there regardless of the slop value.
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    const bool rtl = I18N.isRtl();
    const Geometry geometry = computeGeometry();
    const int pageStart = AppsGridLayout::pageStartOf(selectorIndex);
    const int itemsOnPage = std::min(AppsGridLayout::ITEMS_PER_PAGE, count - pageStart);
    const int touchSlop = mappedInput.hasTouch() ? geometry.gutter / 2 : 0;
    const int hitIndexInPage =
        AppsGridLayout::hitTestTile(tapX, tapY, geometry.gridStartX, geometry.contentTop, geometry.tileWidth,
                                    geometry.tileHeight, geometry.gutter, rtl, itemsOnPage, touchSlop);
    if (hitIndexInPage >= 0) {
      selectorIndex = pageStart + hitIndexInPage;
      if (!launch(entries()[selectorIndex])) requestUpdate();  // stayed here; repaint over the popup
      return;
    }
  }

  // Index order stays the same under RTL (app 0 is still the first app), but the
  // grid renders mirrored -- see render()'s column computation -- so increasing
  // the index moves the visual selection LEFT, not right. Swap which physical
  // button drives which index direction to match, same idiom RecentBooksActivity
  // uses for its own mirrored grid.
  const bool rtl = I18N.isRtl();
  auto moveRight = [this, count] {
    selectorIndex = GridNav::moveHorizontal(selectorIndex, count, true);
    requestUpdate();
  };
  auto moveLeft = [this, count] {
    selectorIndex = GridNav::moveHorizontal(selectorIndex, count, false);
    requestUpdate();
  };
  if (rtl) {
    buttonNavigator.onRelease({MappedInputManager::Button::Right}, moveLeft);
    buttonNavigator.onRelease({MappedInputManager::Button::Left}, moveRight);
  } else {
    buttonNavigator.onRelease({MappedInputManager::Button::Right}, moveRight);
    buttonNavigator.onRelease({MappedInputManager::Button::Left}, moveLeft);
  }
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this, count] {
    selectorIndex =
        GridNav::moveVertical(selectorIndex, count, AppsGridLayout::COLUMNS, AppsGridLayout::ITEMS_PER_PAGE, true);
    requestUpdate();
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this, count] {
    selectorIndex =
        GridNav::moveVertical(selectorIndex, count, AppsGridLayout::COLUMNS, AppsGridLayout::ITEMS_PER_PAGE, false);
    requestUpdate();
  });
}

AppsActivity::Geometry AppsActivity::computeGeometry() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Below the header, not a fixed offset: at 60 the first row of tiles drew straight
  // over the title.
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Fixed at AppsGridLayout::COLUMNS (2), unlike the OPDS/My Books grids, which size
  // their column count off a minimum cell width -- a static 2x3 shape is what makes a
  // predictable page size (and the pagination indicator below) possible. Width still
  // fills the panel edge-to-edge on both the X4 (480) and the wider X3 (528).
  const int tileWidth = (pageWidth - GUTTER * (AppsGridLayout::COLUMNS + 1)) / AppsGridLayout::COLUMNS;
  const int contentHeight = pageHeight - contentTop - BOTTOM_MARGIN - PAGINATION_HEIGHT;
  const int rowHeight = std::max(1, contentHeight / AppsGridLayout::ROWS);
  const int tileHeight = std::max(1, rowHeight - GUTTER);

  const int totalGridWidth = AppsGridLayout::COLUMNS * (tileWidth + GUTTER) - GUTTER;
  const int gridStartX = std::max(0, (pageWidth - totalGridWidth) / 2);

  return Geometry{gridStartX, contentTop, tileWidth, tileHeight, GUTTER};
}

void AppsActivity::drawAppTile(const int cellX, const int cellY, const int tileWidth, const int tileHeight,
                               const uint8_t* icon, const std::string& label, const bool selected,
                               const bool rtl) const {
  // Thin border on every tile, selected or not -- previously only the selected
  // tile had any frame at all, which left unselected tiles looking un-clickable.
  renderer.drawRect(cellX, cellY, tileWidth, tileHeight, 1, true);

  const int labelFontId = UI_10_FONT_ID;
  const int lineHeight = renderer.getLineHeight(labelFontId);
  const int contentHeight = ICON_SIZE + ICON_LABEL_GAP + lineHeight;
  const int iconY = cellY + (tileHeight - contentHeight) / 2;
  const int iconX = cellX + (tileWidth - ICON_SIZE) / 2;
  renderer.drawIcon(icon, iconX, iconY, ICON_SIZE);

  const int maxTextWidth = tileWidth - 8;
  const std::string shown = renderer.truncatedText(labelFontId, label.c_str(), maxTextWidth);
  const int textWidth = renderer.getTextWidth(labelFontId, shown.c_str());
  const int textX = cellX + (tileWidth - textWidth) / 2;
  const int textY = iconY + ICON_SIZE + ICON_LABEL_GAP;
  renderer.drawText(labelFontId, textX, textY, shown.c_str());

  if (!selected) return;

  // Selection ring, drawn just outside the tile's own border: a thick outer
  // stroke plus a thinner nested one ("thick double border"), a solid indicator
  // bar on the tile's leading edge (physically left in LTR, right in RTL -- the
  // reading-direction "this one" edge, same mirroring idiom as drawTabBar's
  // underline), and a focus arrowhead pointing from the bar into the tile.
  const int ringX = cellX - RING_MARGIN;
  const int ringY = cellY - RING_MARGIN;
  const int ringW = tileWidth + 2 * RING_MARGIN;
  const int ringH = tileHeight + 2 * RING_MARGIN;
  renderer.drawRect(ringX, ringY, ringW, ringH, RING_OUTER_WIDTH, true);
  renderer.drawRect(ringX + RING_INNER_INSET, ringY + RING_INNER_INSET, ringW - 2 * RING_INNER_INSET,
                    ringH - 2 * RING_INNER_INSET, 1, true);

  const int indicatorX = rtl ? ringX + ringW - INDICATOR_WIDTH : ringX;
  renderer.fillRect(indicatorX, ringY, INDICATOR_WIDTH, ringH, true);

  // Arrowhead: a stack of vertical lines shrinking from a full-height base (at
  // the indicator bar) down to a single point -- the same hand-drawn-triangle
  // technique BaseTheme::drawList uses for its page-arrow glyphs, rotated 90deg
  // to point sideways instead of up/down.
  const int arrowCenterY = cellY + tileHeight / 2;
  const int arrowBaseX = rtl ? indicatorX - 1 : indicatorX + INDICATOR_WIDTH + 1;
  for (int i = 0; i <= ARROW_HALF_HEIGHT; i++) {
    const int halfLen = ARROW_HALF_HEIGHT - i;
    const int x = rtl ? arrowBaseX - i : arrowBaseX + i;
    if (halfLen > 0) {
      renderer.drawLine(x, arrowCenterY - halfLen, x, arrowCenterY + halfLen, true);
    } else {
      renderer.drawPixel(x, arrowCenterY, true);
    }
  }
}

void AppsActivity::drawPagination(const int centerX, const int y, const int currentPage, const int pageCount) const {
  if (pageCount <= 1) return;

  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d / %d", currentPage + 1, pageCount);
  const int fontId = SMALL_FONT_ID;
  const int textWidth = renderer.getTextWidth(fontId, buf);
  const int lineHeight = renderer.getLineHeight(fontId);
  renderer.drawText(fontId, centerX - textWidth / 2, y, buf);

  // Chevrons flanking the page label. Under RTL the whole reading flow mirrors
  // (page 1 is still first, but "forward" points left), so which glyph sits on
  // which side swaps -- same mirroring idiom as the button hints' Left/Right
  // relabeling below.
  const bool rtl = I18N.isRtl();
  constexpr int CHEVRON_GAP = 10;
  constexpr int CHEVRON_HALF = 4;
  const int chevronY = y + lineHeight / 2;
  const auto drawChevron = [&](const int tipX, const bool pointsRight) {
    for (int i = 0; i <= CHEVRON_HALF; i++) {
      const int halfLen = CHEVRON_HALF - i;
      const int x = pointsRight ? tipX - i : tipX + i;
      if (halfLen > 0) {
        renderer.drawLine(x, chevronY - halfLen, x, chevronY + halfLen, true);
      } else {
        renderer.drawPixel(x, chevronY, true);
      }
    }
  };
  const int leftX = centerX - textWidth / 2 - CHEVRON_GAP;
  const int rightX = centerX + textWidth / 2 + CHEVRON_GAP;
  drawChevron(leftX, rtl);
  drawChevron(rightX, !rtl);
}

void AppsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool rtl = I18N.isRtl();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CAT_APPS));

  const Geometry geometry = computeGeometry();
  const int count = static_cast<int>(entries().size());
  const int pageStart = AppsGridLayout::pageStartOf(selectorIndex);
  const int itemsOnPage = std::min(AppsGridLayout::ITEMS_PER_PAGE, count - pageStart);

  for (int i = 0; i < itemsOnPage; i++) {
    const int idx = pageStart + i;
    const auto& entry = entries()[idx];
    const int logicalCol = AppsGridLayout::colInPage(i);
    const int row = AppsGridLayout::rowInPage(i);
    const int visualCol = AppsGridLayout::mirroredColumn(logicalCol, rtl);
    const int cellX = geometry.gridStartX + visualCol * (geometry.tileWidth + geometry.gutter);
    const int cellY = geometry.contentTop + row * (geometry.tileHeight + geometry.gutter);

    const std::string tileLabel = I18N.get(entry.label);
    drawAppTile(cellX, cellY, geometry.tileWidth, geometry.tileHeight, entry.icon, tileLabel, idx == selectorIndex,
                rtl);
  }

  drawPagination(pageWidth / 2, pageHeight - BOTTOM_MARGIN - PAGINATION_HEIGHT + 4,
                 AppsGridLayout::pageIndexOf(selectorIndex), AppsGridLayout::pageCount(count));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), true);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
