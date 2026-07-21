#include "GymDayDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "GymExerciseBrowserActivity.h"
#include "GymPlanStore.h"
#include "GymWorkoutActivity.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

GymDayDetailActivity::GymDayDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const size_t dayIndex)
    : Activity("GymDayDetail", renderer, mappedInput), dayIndex_(dayIndex) {}

bool GymDayDetailActivity::hasStartRow() const { return !GYM_PLAN.getDay(dayIndex_).exercises.empty(); }

void GymDayDetailActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  requestUpdate();
}

void GymDayDetailActivity::startWorkoutPressed() {
  startActivityForResult(std::make_unique<GymWorkoutActivity>(renderer, mappedInput, dayIndex_),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void GymDayDetailActivity::addExercisePressed() {
  startActivityForResult(std::make_unique<GymExerciseBrowserActivity>(renderer, mappedInput, dayIndex_),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void GymDayDetailActivity::promptRemoveSelected() {
  const auto& day = GYM_PLAN.getDay(dayIndex_);
  const size_t exerciseIndex = static_cast<size_t>(selectedIndex_ - rowOffset());
  if (exerciseIndex >= day.exercises.size()) return;
  const std::string name = day.exercises[exerciseIndex].name;

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), name),
                         [this, exerciseIndex](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             GYM_PLAN.removeExerciseFromDay(dayIndex_, exerciseIndex);
                             const int totalRows =
                                 static_cast<int>(GYM_PLAN.getDay(dayIndex_).exercises.size()) + rowOffset();
                             if (selectedIndex_ >= totalRows) selectedIndex_ = totalRows - 1;
                           }
                           requestUpdate();
                         });
}

void GymDayDetailActivity::loop() {
  if (longPressFired_) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired_ = false;
    }
    return;
  }

  const auto& day = GYM_PLAN.getDay(dayIndex_);
  const int offset = rowOffset();
  const int totalRows = static_cast<int>(day.exercises.size()) + offset;

  if (editingTargets_) {
    const size_t exerciseIndex = static_cast<size_t>(selectedIndex_ - offset);
    if (exerciseIndex >= day.exercises.size()) {
      editingTargets_ = false;
      return;
    }
    const PlannedExercise& ex = day.exercises[exerciseIndex];
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      GYM_PLAN.updateExerciseTargets(dayIndex_, exerciseIndex, ex.targetSets + 1, ex.targetReps);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      GYM_PLAN.updateExerciseTargets(dayIndex_, exerciseIndex, ex.targetSets > 1 ? ex.targetSets - 1 : 1,
                                     ex.targetReps);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      GYM_PLAN.updateExerciseTargets(dayIndex_, exerciseIndex, ex.targetSets, ex.targetReps + 1);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      GYM_PLAN.updateExerciseTargets(dayIndex_, exerciseIndex, ex.targetSets,
                                     ex.targetReps > 1 ? ex.targetReps - 1 : 1);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      editingTargets_ = false;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Long-press Confirm on a real exercise row (never Start Workout/Add
  // Exercise): prompt removal, mirroring RecentBooksActivity's convention.
  if (selectedIndex_ >= offset && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired_ = true;
    promptRemoveSelected();
    return;
  }

  buttonNavigator_.onScrollNextRelease([this, totalRows] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, totalRows);
    requestUpdate();
  });
  buttonNavigator_.onScrollPreviousRelease([this, totalRows] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, totalRows);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (offset == 2 && selectedIndex_ == 0) {
      startWorkoutPressed();
    } else if (selectedIndex_ == offset - 1) {
      addExercisePressed();
    } else {
      editingTargets_ = true;
      requestUpdate();
    }
  }
}

void GymDayDetailActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  char headerBuf[16];
  snprintf(headerBuf, sizeof(headerBuf), tr(STR_GYM_DAY_FORMAT), static_cast<int>(dayIndex_) + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const auto& day = GYM_PLAN.getDay(dayIndex_);
  const int offset = rowOffset();
  const int totalRows = static_cast<int>(day.exercises.size()) + offset;
  const bool startRow = hasStartRow();

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalRows, selectedIndex_,
      [&day, offset, startRow](int i) {
        if (startRow && i == 0) return std::string(tr(STR_START_WORKOUT));
        if (i == offset - 1) return std::string(tr(STR_ADD_EXERCISE));
        return day.exercises[static_cast<size_t>(i - offset)].name;
      },
      [&day, offset, startRow](int i) {
        if (startRow && i == 0) return std::string();
        if (i == offset - 1) return std::string();
        return day.exercises[static_cast<size_t>(i - offset)].bodyPart;
      },
      nullptr,
      [&day, offset, startRow](int i) {
        if (startRow && i == 0) return std::string();
        if (i == offset - 1) return std::string();
        const auto& ex = day.exercises[static_cast<size_t>(i - offset)];
        char buf[16];
        snprintf(buf, sizeof(buf), tr(STR_SETS_REPS_FORMAT), ex.targetSets, ex.targetReps);
        return std::string(buf);
      },
      false);

  if (!day.exercises.empty()) {
    // Separates the action rows (Start Workout / Add Exercise) from the real
    // exercise list below them -- same divider technique FileBrowserActivity
    // uses under its synthetic File Transfer row. Every row here has a
    // (possibly empty) subtitle callback, so drawList uses the TALLER
    // with-subtitle row pitch throughout -- listRowHeight alone landed the
    // divider a full row too early, cutting through the Add Exercise row.
    const int dividerY = contentTop + offset * metrics.listWithSubtitleRowHeight;
    renderer.drawLine(0, dividerY, pageWidth - 1, dividerY, 3, true);
  }

  if (day.exercises.empty()) {
    // The Add Exercise row is always present, so this isn't a true empty
    // state -- just a hint under it that there's nothing else here yet
    // (or, if marked a Rest Day, that it's intentionally empty).
    const char* emptyMsg = day.isRestDay ? tr(STR_REST_DAY) : tr(STR_NO_EXERCISES_YET);
    const int msgWidth = renderer.getTextWidth(SMALL_FONT_ID, emptyMsg);
    renderer.drawText(SMALL_FONT_ID, (pageWidth - msgWidth) / 2, contentTop + metrics.listWithSubtitleRowHeight + 10,
                      emptyMsg);
  }

  if (editingTargets_) {
    GUI.drawSideButtonHints(renderer, tr(STR_SET_PLUS), tr(STR_SET_MINUS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_REP_MINUS), tr(STR_REP_PLUS),
                                              /*rtlSwap=*/false);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // Confirm's hint names the actual action for the highlighted row instead
    // of a generic "Select" -- Start Workout / Add Exercise / edit targets
    // are three quite different outcomes for the same button.
    const char* confirmLabel;
    if (startRow && selectedIndex_ == 0) {
      confirmLabel = tr(STR_START);
    } else if (selectedIndex_ == offset - 1) {
      confirmLabel = tr(STR_ADD);
    } else {
      confirmLabel = tr(STR_EDIT);
    }
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
