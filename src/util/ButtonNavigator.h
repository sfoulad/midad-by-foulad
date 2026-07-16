#pragma once

#include <functional>
#include <vector>

#include "MappedInputManager.h"

class ButtonNavigator final {
  using Callback = std::function<void()>;
  using Buttons = std::vector<MappedInputManager::Button>;

  const uint16_t continuousStartMs;
  const uint16_t continuousIntervalMs;
  uint32_t lastContinuousNavTime = 0;
  static const MappedInputManager* mappedInput;

  [[nodiscard]] bool shouldNavigateContinuously() const;

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(const Callback& callback);
  void onPrevious(const Callback& callback);
  void onPressAndContinuous(const Buttons& buttons, const Callback& callback);

  void onNextPress(const Callback& callback);
  void onPreviousPress(const Callback& callback);
  void onPress(const Buttons& buttons, const Callback& callback);

  void onNextRelease(const Callback& callback);
  void onPreviousRelease(const Callback& callback);
  void onRelease(const Buttons& buttons, const Callback& callback);

  void onNextContinuous(const Callback& callback);
  void onPreviousContinuous(const Callback& callback);
  void onContinuous(const Buttons& buttons, const Callback& callback);

  // Same Press/Release/Continuous family as onNext*/onPrevious*, but bound to
  // Button::ScrollNext/ScrollPrevious instead of NavNext/NavPrevious -- for
  // screens hinting a purely vertical up/down list-scroll pair (see
  // MappedInputManager::mapButton's ScrollNext/ScrollPrevious cases for why
  // that must skip NavNext/NavPrevious's RTL horizontal flip).
  void onScrollNext(const Callback& callback);
  void onScrollPrevious(const Callback& callback);
  void onScrollNextPress(const Callback& callback);
  void onScrollPreviousPress(const Callback& callback);
  void onScrollNextRelease(const Callback& callback);
  void onScrollPreviousRelease(const Callback& callback);
  void onScrollNextContinuous(const Callback& callback);
  void onScrollPreviousContinuous(const Callback& callback);

  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems);

  [[nodiscard]] static int nextPageIndex(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndex(int currentIndex, int totalItems, int itemsPerPage);

  // Navigation uses the logical NavNext / NavPrevious buttons; MappedInputManager::mapButton resolves
  // them to physical buttons and applies any orientation-based direction swap, so this stays settings-free.
  [[nodiscard]] static Buttons getNextButtons() { return {MappedInputManager::Button::NavNext}; }
  [[nodiscard]] static Buttons getPreviousButtons() { return {MappedInputManager::Button::NavPrevious}; }
  [[nodiscard]] static Buttons getScrollNextButtons() { return {MappedInputManager::Button::ScrollNext}; }
  [[nodiscard]] static Buttons getScrollPreviousButtons() { return {MappedInputManager::Button::ScrollPrevious}; }
};
