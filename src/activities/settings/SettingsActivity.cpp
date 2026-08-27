#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ArabicFontSelectionActivity.h"
#include "ArabicFontSystem.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "FouladEbooksConfig.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "MidadSettingsList.h"
#include "OpdsServerListActivity.h"
#include "OpdsServerStore.h"
#include "QuranBook.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "StatusBarSettingsActivity.h"
#include "activities/apps/DictionaryActivity.h"
#include "activities/browser/FouladLogoutActivity.h"
#include "activities/browser/FouladQrLoginActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {
    StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER, StrId::STR_CAT_CONTROLS, StrId::STR_CAT_APPS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  appsSettings.clear();
  systemSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  for (auto& setting : getSettingsList(&sdFontSystem.registry())) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    // Drop a row whose parent setting makes it meaningless, rather than showing it
    // inert -- see SettingInfo::shownWhen. Declared beside each setting now instead
    // of tested here, which is where the one pre-existing case of this lived
    // (pwrBtnFootnoteBack against shortPwrBtn). This function runs after every
    // change, so flipping a parent updates the list on the next repaint.
    if (setting.visibleWhen && !setting.visibleWhen()) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_APPS) {
      appsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  appsSettings.push_back(SettingInfo::Action(StrId::STR_DICTIONARY, SettingAction::Dictionary));
  appsSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  // Midad-owned Apps rows (Quran/Games/Tasbih/Stop Watch/Pomodoro/Gym/Debug
  // Logging) -- see src/MidadSettingsList.h. Appended here, after KOReader
  // Sync, so Debug Logging (the last row that function adds) keeps landing
  // right after KOReader Sync, matching prior behavior.
  // Filtered the same way as the loop above, which it deliberately runs after:
  // appending straight into appsSettings let every Midad row bypass visibleWhen,
  // so the Pomodoro durations and the Gym weight unit stayed on screen with their
  // app switched off (reported). Build them in a scratch list and copy across only
  // what should show. The upstream rows keep their filter in the loop; this is the
  // one other place rows enter a category vector carrying a predicate.
  std::vector<SettingInfo> midadAppRows;
  appendMidadAppSettings(midadAppRows);
  for (auto& setting : midadAppRows) {
    if (setting.visibleWhen && !setting.visibleWhen()) continue;
    appsSettings.push_back(std::move(setting));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  // OPDS Servers is deliberately not listed. Adding a catalog by hand means typing a
  // URL and credentials on an on-screen keyboard, and every catalog these devices
  // actually reach is reached by signing in -- so the row only ever offered a way to
  // get it wrong. The activity itself stays (ActivityManager still routes to it), and
  // SettingAction::OPDSBrowser with it, so nothing else has to change to bring the row
  // back.
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  // OTA fetches this board's own release asset (see OtaUpdater); boards whose
  // asset isn't published yet just report no update available.
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  // One slot, two states: Logout when an account is stored, Login when not.
  // Top of Apps, not System (user request): signing in to Midad is how you reach
  // your library, which is a thing you *use*, not a device setting -- and buried
  // under System it was hard to find for exactly the people who had not signed in
  // yet. insert() at begin() rather than push_back so it stays above Dictionary
  // and KOReader Sync, which are appended above.
  const auto& opdsServers = OPDS_STORE.getServers();
  const bool hasFouladEbooksAccount = std::any_of(
      opdsServers.begin(), opdsServers.end(), [](const OpdsServer& server) { return server.url == FOULAD_EBOOKS_URL; });
  appsSettings.insert(appsSettings.begin(),
                      hasFouladEbooksAccount
                          ? SettingInfo::Action(StrId::STR_FOULAD_EBOOKS_LOGOUT, SettingAction::FouladEbooksLogout)
                          : SettingInfo::Action(StrId::STR_FOULAD_EBOOKS_LOGIN, SettingAction::FouladEbooksLogin));
  // User request: Browse Files / File Transfer are the two most-used System
  // entries, so pin them to the very top regardless of everything else
  // appended above -- inserted last (not where they'd naturally fall in the
  // push_back order) so this stays correct even if more entries are added
  // before this point later.
  systemSettings.insert(systemSettings.begin(),
                        {SettingInfo::Action(StrId::STR_BROWSE_FILES, SettingAction::BrowseFiles),
                         SettingInfo::Action(StrId::STR_FILE_TRANSFER, SettingAction::FileTransfer)});
  // Reader list order (user-specified): Manage Fonts first (the most-used
  // entry -- downloading/removing fonts from the Foulad eBooks store), then
  // English Font, English Font Size, Arabic Font, Arabic Font Size, then the
  // rest. The base table supplies [English Font, English Font Size, Arabic
  // Font Size, ...]; insert "Arabic Font" at index 2 so the two Arabic
  // settings pair up in the same family-then-size order as the English pair.
  // Built via buildArabicFontFamilySetting() (an ENUM, not a plain Action) so
  // the current font's name shows inline in the list -- see that function's
  // comment for why.
  readerSettings.insert(readerSettings.begin() + 2, buildArabicFontFamilySetting(&arabicFontSystem.registry()));
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &appsSettings;
      break;
    case 4:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  // Handle navigation
  buttonNavigator.onScrollNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onScrollPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onScrollNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  buttonNavigator.onScrollPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
      case 0:
        currentSettings = &displaySettings;
        break;
      case 1:
        currentSettings = &readerSettings;
        break;
      case 2:
        currentSettings = &controlsSettings;
        break;
      case 3:
        currentSettings = &appsSettings;
        break;
      case 4:
        currentSettings = &systemSettings;
        break;
    }
    settingsCount = static_cast<int>(currentSettings->size());
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;
  const bool arabicFontSizeChanged = setting.valuePtr == &CrossPointSettings::arabicFontSize;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::TOGGLE && setting.valueGetter && setting.valueSetter) {
    // Same as the valuePtr branch above, for a field stored outside
    // CrossPointSettings (e.g. MidadAppSettings) -- see SettingInfo::DynamicToggle.
    const uint8_t currentValue = setting.valueGetter();
    setting.valueSetter(currentValue ? 0 : 1);
    if (setting.nameId == StrId::STR_QURAN && !currentValue) {
      // Enabling the Quran extracts the firmware-embedded EPUB to the SD card
      // (a few seconds on first enable; instant when already extracted).
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      if (!QuranBook::ensureExtracted()) {
        setting.valueSetter(0);  // no SD / write failure: stay honest, keep it off
      }
      requestUpdate();
    }
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (setting.enumValues.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                       currentValue,
                       [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged, arabicFontSizeChanged](int idx) {
                         SETTINGS.*valuePtr = idx;
                         syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
                         if (arabicFontSizeChanged) {
                           // Re-resolve the built-in Arabic reading font at the new size (only
                           // takes effect when no SD Arabic font override is active -- see
                           // ArabicFontSystem::ensureLoaded).
                           arabicFontSystem.ensureLoaded(renderer);
                         }
                         SETTINGS.saveToFile();
                         rebuildSettingsLists();
                       });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      // Launch font selection submenu instead of cycling
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    if (setting.nameId == StrId::STR_ARABIC_FONT) {
      // Launch the Arabic font picker instead of cycling, same reasoning as Font Family
      // above -- it also has a live glyph preview pane, which matters more here since the
      // built-in Arabic styles look meaningfully different from each other.
      startActivityForResult(
          std::make_unique<ArabicFontSelectionActivity>(renderer, mappedInput, &arabicFontSystem.registry()),
          [this](const ActivityResult&) {
            SETTINGS.saveToFile();
            rebuildSettingsLists();
          });
      return;
    }
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues > 2) {
      const auto valueSetter = setting.valueSetter;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
        valueSetter(idx);
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::VALUE && setting.valueGetter && setting.valueSetter) {
    // Same as the valuePtr branch above, for a field stored outside
    // CrossPointSettings -- see SettingInfo::DynamicValue.
    const int currentValue = setting.valueGetter();
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      setting.valueSetter(setting.valueRange.min);
    } else {
      setting.valueSetter(static_cast<uint8_t>(currentValue + setting.valueRange.step));
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Dictionary:
        startActivityForResult(std::make_unique<DictionaryActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        // Settings -> Wi-Fi Networks is the ONE WiFi surface with no parent
        // flow and no exit reboot: WifiSelectionActivity deliberately leaves
        // the connection to its caller, and every other caller reboots (which
        // powers the modem off). Coming back here, fully power the radio down
        // or it idles in STA mode (~20-30 mA) until the next deep sleep --
        // confirmed as the fast-battery-drain path (user report). Saved
        // credentials auto-reconnect the next time a flow needs WiFi.
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                               [this](const ActivityResult& result) {
                                 if (WiFi.getMode() != WIFI_MODE_NULL) {
                                   WiFi.disconnect(true);
                                   WiFi.mode(WIFI_OFF);
                                   LOG_INF("SETTINGS", "WiFi radio powered off after network screen");
                                 }
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                                 requestUpdate();
                               });
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        // Reboot into the OTA flow instead of opening it in this session: after
        // Home/library browsing the heap is fragmented below what the GitHub TLS
        // handshakes need (see silentRestartToOtaCheck). Does not return.
        silentRestartToOtaCheck();
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::BrowseFiles:
        startActivityForResult(std::make_unique<FileBrowserActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::FileTransfer:
        // Reboot into the web server on a fresh heap (see SilentRestart.h):
        // starting it from a long-running session leaves the WiFi driver and
        // TCP buffers fighting a fragmented heap and page loads crawl. The
        // activity's own exit path already silentRestart()s back to Home.
        silentRestartToFileTransfer();
        break;
      case SettingAction::FouladEbooksLogout: {
        // Two stages: confirm, then have the server actually drop this device. The
        // credential is only cleared once the server says so -- previously this was
        // local-only, so signing out left the unit listed under "My Devices" with a
        // token that still authenticated.
        auto removedHandler = [this](const ActivityResult& logoutResult) {
          if (!logoutResult.isCancelled) {
            auto& servers = OPDS_STORE.getServers();
            for (size_t i = 0; i < servers.size(); i++) {
              if (servers[i].url == FOULAD_EBOOKS_URL) {
                OPDS_STORE.removeServer(i);
                break;
              }
            }
          }
          rebuildSettingsLists();
          selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
        };

        auto logoutHandler = [this, removedHandler](const ActivityResult& result) {
          if (result.isCancelled) {
            rebuildSettingsLists();
            selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
            return;
          }
          // Credentials are read here rather than inside the activity so it stays a
          // pure "remove this device" step with no knowledge of OpdsServerStore.
          std::string username;
          std::string password;
          for (const auto& server : OPDS_STORE.getServers()) {
            if (server.url == FOULAD_EBOOKS_URL) {
              username = server.username;
              password = server.password;
              break;
            }
          }
          startActivityForResult(
              std::make_unique<FouladLogoutActivity>(renderer, mappedInput, std::move(username), std::move(password)),
              removedHandler);
        };
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_FOULAD_EBOOKS_LOGOUT_CONFIRM), ""),
            logoutHandler);
        break;
      }
      case SettingAction::FouladEbooksLogin:
        // Same QR sign-in as the first-run home entry. On success it stores the
        // issued token and silent-reboots into the catalog on a fresh heap; on
        // cancel we return here, so rebuild the list either way to flip this row
        // between Login and Logout.
        startActivityForResult(std::make_unique<FouladQrLoginActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 rebuildSettingsLists();
                                 selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
                               });
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::TOGGLE && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        } else if (setting.type == SettingType::VALUE && setting.valueGetter) {
          valueText = std::to_string(setting.valueGetter());
        }
        return valueText;
      },
      true);

  // Draw help text
  const auto confirmLabel =
      (selectedSettingIndex == 0)
          ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
          : (selectedSettingIndex > 0 && (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_TIME_TO_SLEEP
                 ? tr(STR_SELECT)
                 : tr(STR_TOGGLE));

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
