#include "MidadSettingsExtension.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "FouladEbooksConfig.h"
#include "MidadSettingsList.h"
#include "OpdsServerStore.h"
#include "SilentRestart.h"
#include "activities/Activity.h"
#include "activities/apps/DictionaryActivity.h"
#include "activities/browser/FouladLogoutActivity.h"
#include "activities/browser/FouladQrLoginActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/settings/SettingsExtension.h"
#include "activities/util/ConfirmationActivity.h"

// main.cpp's globals. An extension handler is handed the hosting Activity so
// it can call startActivityForResult(), but Activity::renderer/mappedInput are
// protected -- these are the same two objects every activity holds a reference
// to, so a child screen built from them is the screen the host would build.
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {

bool hasFouladEbooksAccount() {
  const auto& servers = OPDS_STORE.getServers();
  return std::any_of(servers.begin(), servers.end(),
                     [](const OpdsServer& server) { return server.url == FOULAD_EBOOKS_URL; });
}

void openDictionary(Activity& host) {
  host.startActivityForResult(std::make_unique<DictionaryActivity>(renderer, mappedInputManager),
                              [](const ActivityResult&) {});
}

void openBrowseFiles(Activity& host) {
  host.startActivityForResult(std::make_unique<FileBrowserActivity>(renderer, mappedInputManager),
                              [](const ActivityResult&) {});
}

void startFileTransfer(Activity&) {
  // Reboot into the web server on a fresh heap (see SilentRestart.h): started
  // from a long-running session the WiFi driver and TCP buffers fight a
  // fragmented heap and page loads crawl. Does not return.
  silentRestartToFileTransfer();
}

void openFouladEbooksLogin(Activity& host) {
  // Same QR sign-in as the first-run home entry. On success it stores the
  // issued token and silent-reboots into the catalog; on cancel we come back
  // here and the provider's next run flips this row to Logout.
  host.startActivityForResult(std::make_unique<FouladQrLoginActivity>(renderer, mappedInputManager),
                              [](const ActivityResult&) {});
}

void openFouladEbooksLogout(Activity& host) {
  // Two stages: confirm, then have the server actually drop this device. The
  // stored credential is only cleared once the server says so, or the unit
  // stays listed under "My Devices" with a token that still authenticates.
  auto removedHandler = [](const ActivityResult& logoutResult) {
    if (logoutResult.isCancelled) return;
    auto& servers = OPDS_STORE.getServers();
    for (size_t i = 0; i < servers.size(); i++) {
      if (servers[i].url == FOULAD_EBOOKS_URL) {
        OPDS_STORE.removeServer(i);
        break;
      }
    }
  };

  auto confirmHandler = [removedHandler, &host](const ActivityResult& result) {
    if (result.isCancelled) return;
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
    host.startActivityForResult(std::make_unique<FouladLogoutActivity>(renderer, mappedInputManager,
                                                                      std::move(username), std::move(password)),
                                removedHandler);
  };

  host.startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInputManager, tr(STR_FOULAD_EBOOKS_LOGOUT_CONFIRM), ""),
      confirmHandler);
}

}  // namespace

std::vector<SettingsExtensionCategory> midadSettingsExtensionProvider() {
  SettingsExtensionCategory apps;
  apps.label = I18N.get(StrId::STR_CAT_APPS);
  // 4 action rows + the ~11 MidadAppSettings rows appendMidadAppSettings adds.
  apps.settings.reserve(16);

  // One slot, two states: Logout when an account is stored, Login when not.
  // Top of the tab -- signing in to Midad is how you reach your library, and
  // it is exactly the people who have not signed in yet who need to find it.
  if (hasFouladEbooksAccount()) {
    apps.settings.push_back(
        SettingInfo::ExtensionAction(&openFouladEbooksLogout).withLabel(I18N.get(StrId::STR_FOULAD_EBOOKS_LOGOUT)));
  } else {
    apps.settings.push_back(
        SettingInfo::ExtensionAction(&openFouladEbooksLogin).withLabel(I18N.get(StrId::STR_FOULAD_EBOOKS_LOGIN)));
  }
  apps.settings.push_back(SettingInfo::ExtensionAction(&openDictionary).withLabel(I18N.get(StrId::STR_DICTIONARY)));
  apps.settings.push_back(SettingInfo::ExtensionAction(&openBrowseFiles).withLabel(I18N.get(StrId::STR_BROWSE_FILES)));
  apps.settings.push_back(
      SettingInfo::ExtensionAction(&startFileTransfer).withLabel(I18N.get(StrId::STR_FILE_TRANSFER)));

  // Quran / Games / Tasbih / Stop Watch / Pomodoro / Gym / Debug Logging.
  // Already DynamicToggle/DynamicValue/DynamicEnum rows, which is exactly what
  // the extension point consumes -- no adapter needed.
  appendMidadAppSettings(apps.settings);

  std::vector<SettingsExtensionCategory> categories;
  categories.reserve(1);
  categories.push_back(std::move(apps));
  return categories;
}

void registerMidadSettingsExtension() { setSettingsExtensionProvider(&midadSettingsExtensionProvider); }
