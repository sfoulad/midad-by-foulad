#include "SettingsCategoryBuilder.h"

#include <algorithm>

#include "ArabicFontSystem.h"
#include "CrossPointSettings.h"
#include "FouladEbooksConfig.h"
#include "MidadSettingsList.h"
#include "OpdsServerStore.h"
#include "SdCardFontSystem.h"

CategorizedSettings buildCategorizedSettings() {
  CategorizedSettings out;

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran -- otherwise the font-family picker shows a stale list.
  sdFontSystem.refreshIfDirty();

  for (auto& setting : getSettingsList(&sdFontSystem.registry())) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      out.display.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      out.reader.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      out.controls.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_APPS) {
      out.apps.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      out.system.push_back(setting);
    }
  }

  // Append device-only ACTION items
  out.controls.insert(out.controls.begin(),
                      SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  out.apps.push_back(SettingInfo::Action(StrId::STR_DICTIONARY, SettingAction::Dictionary));
  out.apps.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  // Midad-owned Apps rows (Quran/Games/Tasbih/Stop Watch/Pomodoro/Gym/Debug
  // Logging) -- appended here, after KOReader Sync, so Debug Logging (the last
  // row that function adds) keeps landing right after KOReader Sync.
  appendMidadAppSettings(out.apps);
  out.system.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  // OPDS Servers is deliberately not listed -- see the original comment history
  // in SettingsActivity.cpp. The activity itself stays (ActivityManager still
  // routes to it), and SettingAction::OPDSBrowser with it.
  out.system.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  out.system.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  out.system.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  out.system.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  // One slot, two states: Logout when an account is stored, Login when not.
  // insert() at begin() rather than push_back so it stays above Dictionary and
  // KOReader Sync, which are appended above.
  const auto& opdsServers = OPDS_STORE.getServers();
  const bool hasFouladEbooksAccount = std::any_of(
      opdsServers.begin(), opdsServers.end(), [](const OpdsServer& server) { return server.url == FOULAD_EBOOKS_URL; });
  out.apps.insert(out.apps.begin(),
                  hasFouladEbooksAccount
                      ? SettingInfo::Action(StrId::STR_FOULAD_EBOOKS_LOGOUT, SettingAction::FouladEbooksLogout)
                      : SettingInfo::Action(StrId::STR_FOULAD_EBOOKS_LOGIN, SettingAction::FouladEbooksLogin));
  // Browse Files / File Transfer pinned to the very top of System regardless of
  // everything else appended above -- inserted last so this stays correct even
  // if more entries are added before this point later.
  out.system.insert(out.system.begin(),
                    {SettingInfo::Action(StrId::STR_BROWSE_FILES, SettingAction::BrowseFiles),
                     SettingInfo::Action(StrId::STR_FILE_TRANSFER, SettingAction::FileTransfer)});
  // Reader list order: Manage Fonts first, then English Font, English Font
  // Size, Arabic Font, Arabic Font Size, then the rest. The base table
  // supplies [English Font, English Font Size, Arabic Font Size, ...]; insert
  // "Arabic Font" at index 2 so the two Arabic settings pair up in the same
  // family-then-size order as the English pair.
  out.reader.insert(out.reader.begin() + 2, buildArabicFontFamilySetting(&arabicFontSystem.registry()));
  out.reader.insert(out.reader.begin(), SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  out.reader.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  return out;
}
