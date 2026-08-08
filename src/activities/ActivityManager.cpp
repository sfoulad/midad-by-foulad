#include "ActivityManager.h"

#include <FontCacheManager.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>

#include <algorithm>

#include "FouladEbooksConfig.h"
#include "OpdsCoverCache.h"
#include "OpdsServerStore.h"
#include "apps/AppsActivity.h"
#include "apps/DictionaryActivity.h"
#include "apps/GymActivity.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/FouladQrLoginActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

void ActivityManager::begin() {
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          8192,               // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          0                   // Pin to core 0 (PRO_CPU)
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      currentActivity->render(std::move(lock));
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToSettings() { replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput)); }

void ActivityManager::goToGym() { replaceActivity(std::make_unique<GymActivity>(renderer, mappedInput)); }

void ActivityManager::goToDictionary() { replaceActivity(std::make_unique<DictionaryActivity>(renderer, mappedInput)); }

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToRecentBooks() {
  replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
}

void ActivityManager::goToApps() { replaceActivity(std::make_unique<AppsActivity>(renderer, mappedInput)); }

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToFouladEbooks() {
  const auto& servers = OPDS_STORE.getServers();
  const auto it = std::find_if(servers.begin(), servers.end(),
                               [](const OpdsServer& server) { return server.url == FOULAD_EBOOKS_URL; });
  if (it != servers.end()) {
    // Already set up on this device — skip straight to browsing.
    //
    // Clear the cover cache on every entry rather than requiring a manual Settings
    // action: a cover that downloaded successfully once is never re-validated against
    // the server afterward (ensureOpdsCoverCached's "already cached" fast path just
    // checks the BMP is well-formed, not that it's still current), so covers cached
    // from before a server-side URL/scheme change wouldn't otherwise self-heal even
    // after the underlying issue is fixed. Foulad eBooks specifically, not a
    // user-added OPDS server, since only this one has had server-side URL changes
    // during the beta.
    clearOpdsCoverCache();
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, *it));
    return;
  }
  // First time on this device — collect username/password before browsing.
  replaceActivity(std::make_unique<FouladQrLoginActivity>(renderer, mappedInput));
}

void ActivityManager::goToNews() {
  // News is the same browser pointed at a different root, not a second browser:
  // the feed is ordinary OPDS, so listing, downloading, the 401-to-QR path and the
  // WiFi handling all come free. Only the empty state differs, and the activity
  // decides that from the URL (EINK_NEWS_TASKS.md §2).
  const auto& servers = OPDS_STORE.getServers();
  const auto it = std::find_if(servers.begin(), servers.end(),
                               [](const OpdsServer& server) { return server.url == FOULAD_EBOOKS_URL; });
  if (it == servers.end()) {
    // Not paired. The QR screen is the correct News state too, per §2 -- same
    // credential, same pairing, so there is nothing News-specific to sign in to.
    replaceActivity(std::make_unique<FouladQrLoginActivity>(renderer, mappedInput));
    return;
  }
  OpdsServer news = *it;
  news.url = FOULAD_EBOOKS_NEWS_URL;
  news.name = tr(STR_NEWS);
  replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, std::move(news)));
}

void ActivityManager::goToReader(std::string path) {
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem) {
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    // CrossPointWebServer has no Home row to pre-select (it lives under Settings ->
    // System), so it falls through to NONE here.
    if (activityName == "RecentBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    } else if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::FOULAD_EBOOKS;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

const char* ActivityManager::currentActivityDebugName() const {
  return currentActivity ? currentActivity->activityDebugName() : "none";
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

uint8_t ActivityManager::sampleButtonLevel() const {
  uint8_t level = 0;
  if (mappedInput.isPressed(MappedInputManager::Button::Left)) level |= WAIT_BIT_LEFT;
  if (mappedInput.isPressed(MappedInputManager::Button::Right)) level |= WAIT_BIT_RIGHT;
  if (mappedInput.isPressed(MappedInputManager::Button::Up)) level |= WAIT_BIT_UP;
  if (mappedInput.isPressed(MappedInputManager::Button::Down)) level |= WAIT_BIT_DOWN;
  if (mappedInput.isPressed(MappedInputManager::Button::Back)) level |= WAIT_BIT_BACK;
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) level |= WAIT_BIT_CONFIRM;
  return level;
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  bool registeredAsWaiter = false;
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
    registeredAsWaiter = true;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  if (!registeredAsWaiter) {
    // One of the guard conditions above tripped. In a debug build the assert already
    // caught it; in a release build (asserts compiled out) we must not fall through to
    // blocking below -- the render task only ever notifies whichever single handle is
    // recorded in waitingTaskHandle, and we were NOT recorded, so that block would wait
    // forever for a notification that will never arrive. Fall back to a fire-and-forget
    // update instead of deadlocking the caller -- almost always the single foreground
    // task that also processes button input, so a deadlock here reads as the whole
    // device being frozen with no recovery short of a battery pull.
    LOG_ERR("ACT",
            "requestUpdateAndWait: not registered as waiter (alreadyWaiting=%d isRenderTask=%d "
            "holdingRenderLock=%d), falling back to requestUpdate()",
            alreadyWaiting, isRenderTask, holdingRenderLock);
    requestUpdate(/*immediate=*/true);
    return;
  }

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  // Bounded wait, not portMAX_DELAY: if the render task never comes back -- a hang or
  // crash inside the current screen's Activity::render() -- the caller must not block
  // forever, for the same reason as above (it's almost always the single foreground
  // task, so blocking forever here means every button press stops doing anything, with
  // no recovery short of a battery pull; user-reported as Snake "freezing completely" --
  // Snake calls this on every single step, ~every 300ms for as long as a game runs, the
  // most sustained caller of this primitive in the app and the most likely to expose a
  // rare stall). On timeout, drop the wait and let the caller continue -- worst case one
  // stale/missed frame, not a bricked UI.
  // 5000 ticks, not pdMS_TO_TICKS(5000): the host simulator's FreeRTOS shim doesn't
  // define TickType_t/pdMS_TO_TICKS, and on real hardware CONFIG_FREERTOS_HZ=1000
  // (1 tick = 1ms) makes the literal equivalent anyway.
  constexpr uint32_t kRenderWaitTimeoutTicks = 5000;
  // Poll in short slices instead of one big blocking wait. FAST_REFRESH alone
  // takes ~500ms on this hardware (see HalDisplay::begin's own comment) -- often
  // already longer than a caller's own step interval -- and while this task sits
  // blocked here, gpio.update() (button edge polling) never runs; it's only ever
  // called once per outer main.cpp loop() iteration, which this whole wait is
  // nested inside. A full button press+release entirely within that window was
  // silently lost with no trace (confirmed on-device via a diagnostic log: zero
  // presses registered across ~20s of continuous Snake play, every step blocked
  // ~800ms -- "buttons do nothing but the game keeps moving" was exactly what
  // that looks like from outside). Calling gpio.update() again here mid-wait is
  // not a new pattern -- main.cpp's own comment already documents
  // CrossPointWebServerActivity doing the same thing (polling input between
  // handleClient() bursts) for the identical reason.
  constexpr uint32_t kPollSliceTicks = 20;
  uint32_t waitedTicks = 0;
  bool notified = false;
  // Level-based (isPressed(), not wasPressed()) edge detection, entirely local to this
  // function: InputManager::wasPressed()'s edge flags (freeink-sdk, not this repo) get
  // reset on every single update() call and only repopulated on the specific call where a
  // debounced transition lands, so repeatedly polling gpio.update() here without ALSO
  // reading wasPressed() after each individual call would otherwise wipe out a press
  // before the caller ever sees it -- exactly the bug this loop exists to avoid. isPressed()
  // doesn't have that problem (it reflects current button state continuously), so comparing
  // consecutive samples for a 0->1 transition gives a reliable, self-contained edge detector
  // that survives however many polls happen before requestUpdateAndWait() returns.
  uint8_t previousLevel = sampleButtonLevel();
  while (waitedTicks < kRenderWaitTimeoutTicks) {
    if (ulTaskNotifyTake(pdTRUE, kPollSliceTicks) != 0) {
      notified = true;
      break;
    }
    waitedTicks += kPollSliceTicks;
    gpio.update();
    const uint8_t currentLevel = sampleButtonLevel();
    pressesDuringLastWait |= currentLevel & static_cast<uint8_t>(~previousLevel);
    previousLevel = currentLevel;
  }
  if (!notified) {
    LOG_ERR("ACT", "requestUpdateAndWait: timed out waiting for render task notification");
    // Clear our own registration (if it's still ours -- a very late notification could
    // have already done this) so a future call isn't permanently blocked by
    // alreadyWaiting once the render task does eventually come back, if ever.
    taskENTER_CRITICAL(&activityManagerSpinlock);
    if (waitingTaskHandle == currTaskHandler) {
      waitingTaskHandle = nullptr;
    }
    taskEXIT_CRITICAL(&activityManagerSpinlock);
  }
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
