#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "util/ScreenshotInfo.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration

// FILE_BROWSER/FILE_TRANSFER are kept for source compatibility with any saved/serialized
// state, but Home no longer renders or navigates to them directly (moved under Settings ->
// System); see HomeActivity's menuItemToIndex/indexToMenuItem.
enum class HomeMenuItem {
  NONE,
  FILE_BROWSER,
  RECENTS,
  FOULAD_EBOOKS,
  FILE_TRANSFER,
  SETTINGS_MENU,
  CHECK_UPDATE,
  STATS
};

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = PendingAction::None;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  std::atomic<bool> requestedUpdate{false};

  // Buttons that transitioned not-pressed -> pressed at any point during the most recent
  // requestUpdateAndWait() call -- see that method's own comment for why this exists.
  // Deliberately NOT surfaced through MappedInputManager::wasPressed()/wasAnyPressed(): those
  // are read non-destructively in several places today (e.g. EpubReaderActivity's per-iteration
  // "did anything happen" probe, SnakeActivity's own diagnostic logging) specifically so a later
  // real handler in the same loop() call can still see the same press. Folding this into that
  // same sticky state would make those non-destructive reads silently consume it before the
  // caller who actually wants it -- reproducing this exact bug via the fix meant to catch it.
  // This is a separate, opt-in, additive bitmask: nothing reads it unless it explicitly calls
  // consumePressesDuringLastWait(), so it can't interfere with anything existing.
  uint8_t pressesDuringLastWait = 0;
  [[nodiscard]] uint8_t sampleButtonLevel() const;

 public:
  static constexpr uint8_t WAIT_BIT_LEFT = 1 << 0;
  static constexpr uint8_t WAIT_BIT_RIGHT = 1 << 1;
  static constexpr uint8_t WAIT_BIT_UP = 1 << 2;
  static constexpr uint8_t WAIT_BIT_DOWN = 1 << 3;
  static constexpr uint8_t WAIT_BIT_BACK = 1 << 4;
  static constexpr uint8_t WAIT_BIT_CONFIRM = 1 << 5;

  // Returns (and clears) which of Left/Right/Up/Down/Back/Confirm were newly pressed at any
  // point during the most recent requestUpdateAndWait() call. See that method's own comment:
  // while blocked there, gpio.update()'s own internal edge state can be wiped by this
  // function's repeated polling before the caller's normal wasPressed()/onPress() check ever
  // runs (confirmed on-device: a press-and-release entirely inside a slow render wait vanished
  // with no trace -- "buttons do nothing but the game keeps moving"). Callers that need to stay
  // responsive across a blocking wait (e.g. Snake's per-step render wait) should check this in
  // addition to their normal button handling.
  uint8_t consumePressesDuringLastWait() {
    const uint8_t result = pressesDuringLastWait;
    pressesDuringLastWait = 0;
    return result;
  }


  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer();
  void goToSettings();
  void goToFileBrowser(std::string path = {});
  void goToRecentBooks();
  void goToBrowser();
  void goToFouladEbooks();
  void goToReader(std::string path);
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE);

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  const char* currentActivityDebugName() const;
  bool isReaderActivity() const;
  bool skipLoopDelay() const;
  ScreenshotInfo getScreenshotInfo() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
