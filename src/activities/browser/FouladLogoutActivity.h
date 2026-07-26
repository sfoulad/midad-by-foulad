#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * Signs this device out of Foulad eBooks, removing it from the account first.
 *
 * Exists as its own activity rather than living in SettingsActivity's confirmation
 * handler because signing out is now two blocking network round trips (list, then
 * delete). Run inline, those would freeze the settings list for seconds with no
 * indication anything was happening.
 *
 * Result convention matches ConfirmationActivity: isCancelled == false means the
 * server confirmed removal and the caller should now clear the stored credential.
 * isCancelled == true means it did not happen and the credential MUST be kept -- the
 * whole point is that the device never believes it is signed out while the server
 * still lists it.
 */
class FouladLogoutActivity final : public Activity {
  enum class State : uint8_t { Working, NoWifi, Failed };

  std::string username;
  std::string password;
  State state = State::Working;

 public:
  explicit FouladLogoutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string username,
                                std::string password)
      : Activity("FouladLogout", renderer, mappedInput), username(std::move(username)), password(std::move(password)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
