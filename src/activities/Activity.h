#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  // Returns (and clears) buttons newly pressed at any point during the most recent
  // requestUpdateAndWait() call -- see ActivityManager::consumePressesDuringLastWait()'s own
  // comment. For activities (e.g. games) that need to stay responsive across a blocking
  // render wait; check in addition to normal wasPressed()/onPress() handling, not instead of.
  uint8_t consumePressesDuringLastWait() { return activityManager.consumePressesDuringLastWait(); }

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  // Class name for diagnostics only (e.g. SleepDiagLog, when preventAutoSleep() is
  // true) -- a literal per override, not RTTI, so it stays meaningful after mangling.
  virtual const char* activityDebugName() const { return "Activity"; }
  virtual bool isReaderActivity() const { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);
};
