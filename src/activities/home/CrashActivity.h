#pragma once
#include "activities/Activity.h"

class CrashActivity final : public Activity {
  std::string panicMessage;

  // "Send Log" upload flow -- see startSendLog()/onWifiSelectionComplete()/
  // performSend() in the .cpp. Connecting is a distinct state (rather than
  // reusing Idle/Sending) so loop() knows to stop reading input while
  // WifiSelectionActivity owns the screen.
  enum class SendState { Idle, Connecting, Sending, Sent, Failed, NoAccount };
  SendState sendState = SendState::Idle;
  // Foulad eBooks credentials already saved on this device (see
  // OpdsServerStore) -- looked up once when the user taps Send Log, held
  // here since the WiFi-connect round trip happens across multiple loop()
  // iterations.
  std::string sendUsername;
  std::string sendPassword;

  void startSendLog();
  void onWifiSelectionComplete(bool connected);
  void performSend();

 public:
  explicit CrashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Crash", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
