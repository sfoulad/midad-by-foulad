#include "GymExerciseBrowserActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "GymPlanStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Body-part/equipment strings come straight from the source dataset (English
// only, lowercase, e.g. "abdominals", "lower back") -- not run through i18n,
// since translating exercise metadata is well beyond this feature's scope.
// Capitalized only for display, here.
std::string capitalize(const std::string& s) {
  if (s.empty()) return s;
  std::string out = s;
  out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}
}  // namespace

GymExerciseBrowserActivity::GymExerciseBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const size_t dayIndex)
    : Activity("GymExerciseBrowser", renderer, mappedInput), dayIndex_(dayIndex) {}

void GymExerciseBrowserActivity::onEnter() {
  Activity::onEnter();
  GymCatalog::loadFromSd(allExercises_);

  bodyParts_.clear();
  for (const auto& entry : allExercises_) {
    if (std::find(bodyParts_.begin(), bodyParts_.end(), entry.bodyPart) == bodyParts_.end()) {
      bodyParts_.push_back(entry.bodyPart);
    }
  }
  std::sort(bodyParts_.begin(), bodyParts_.end());

  bodyPartCounts_.assign(bodyParts_.size(), 0);
  for (const auto& entry : allExercises_) {
    const auto it = std::find(bodyParts_.begin(), bodyParts_.end(), entry.bodyPart);
    if (it != bodyParts_.end()) {
      ++bodyPartCounts_[static_cast<size_t>(it - bodyParts_.begin())];
    }
  }

  state_ = BODY_PART_LIST;
  bodyPartIndex_ = 0;
  requestUpdate();
}

void GymExerciseBrowserActivity::filterByBodyPart(const std::string& bodyPart) {
  filteredIndices_.clear();
  for (size_t i = 0; i < allExercises_.size(); i++) {
    if (allExercises_[i].bodyPart == bodyPart) filteredIndices_.push_back(i);
  }
}

void GymExerciseBrowserActivity::addSelectedExercise() {
  if (exerciseIndex_ < 0 || static_cast<size_t>(exerciseIndex_) >= filteredIndices_.size()) return;
  const auto& entry = allExercises_[filteredIndices_[static_cast<size_t>(exerciseIndex_)]];

  PlannedExercise ex;
  ex.slug = entry.slug;
  ex.name = entry.name;
  ex.bodyPart = entry.bodyPart;
  // targetSets/targetReps keep PlannedExercise's own defaults (3x10) --
  // adjustable afterward from GymDayDetailActivity's edit mode.

  if (GYM_PLAN.addExerciseToDay(dayIndex_, ex)) {
    // Stay in the exercise list (same body part) instead of finish()-ing all
    // the way back to Day Detail -- building out a full day commonly means
    // adding several exercises from the same muscle group in a row, and
    // re-walking body-part-list -> exercise-list for each one was the
    // biggest source of extra navigation in the original flow.
    GUI.drawPopup(renderer, tr(STR_ADDED));
    renderer.displayBuffer();
    delay(700);
    requestUpdate();
  } else {
    GUI.drawPopup(renderer, tr(STR_DAY_FULL));
    renderer.displayBuffer();
    delay(1200);
    requestUpdate();
  }
}

void GymExerciseBrowserActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (state_ == EXERCISE_LIST) {
      state_ = BODY_PART_LIST;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (state_ == BODY_PART_LIST) {
    const int count = static_cast<int>(bodyParts_.size());
    // Body-part list has no subtitle callback (its 2nd drawList arg is
    // nullptr -- the count is drawn via the value/badge slot instead), so it
    // uses the shorter no-subtitle row height.
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
    buttonNavigator_.onScrollNextRelease([this, count] {
      if (count > 0) {
        bodyPartIndex_ = ButtonNavigator::nextIndex(bodyPartIndex_, count);
        requestUpdate();
      }
    });
    buttonNavigator_.onScrollPreviousRelease([this, count] {
      if (count > 0) {
        bodyPartIndex_ = ButtonNavigator::previousIndex(bodyPartIndex_, count);
        requestUpdate();
      }
    });
    // Long-press-and-hold Up/Down (or front Left/Right) jumps a full page at
    // a time -- with 17 body parts this mostly just skips single-step
    // scrolling, but it becomes essential in the exercise list below where
    // some body parts have 100+ entries.
    buttonNavigator_.onScrollNextContinuous([this, count, pageItems] {
      if (count > 0) {
        bodyPartIndex_ = ButtonNavigator::nextPageIndex(bodyPartIndex_, count, pageItems);
        requestUpdate();
      }
    });
    buttonNavigator_.onScrollPreviousContinuous([this, count, pageItems] {
      if (count > 0) {
        bodyPartIndex_ = ButtonNavigator::previousPageIndex(bodyPartIndex_, count, pageItems);
        requestUpdate();
      }
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && count > 0) {
      filterByBodyPart(bodyParts_[static_cast<size_t>(bodyPartIndex_)]);
      exerciseIndex_ = 0;
      state_ = EXERCISE_LIST;
      requestUpdate();
    }
  } else {
    const int count = static_cast<int>(filteredIndices_.size());
    // Exercise list's 2nd drawList arg is the equipment subtitle, so it uses
    // the taller with-subtitle row height -- must match here or page jumps
    // would land on the wrong row.
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);
    buttonNavigator_.onScrollNextRelease([this, count] {
      if (count > 0) {
        exerciseIndex_ = ButtonNavigator::nextIndex(exerciseIndex_, count);
        requestUpdate();
      }
    });
    buttonNavigator_.onScrollPreviousRelease([this, count] {
      if (count > 0) {
        exerciseIndex_ = ButtonNavigator::previousIndex(exerciseIndex_, count);
        requestUpdate();
      }
    });
    buttonNavigator_.onScrollNextContinuous([this, count, pageItems] {
      if (count > 0) {
        exerciseIndex_ = ButtonNavigator::nextPageIndex(exerciseIndex_, count, pageItems);
        requestUpdate();
      }
    });
    buttonNavigator_.onScrollPreviousContinuous([this, count, pageItems] {
      if (count > 0) {
        exerciseIndex_ = ButtonNavigator::previousPageIndex(exerciseIndex_, count, pageItems);
        requestUpdate();
      }
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      addSelectedExercise();
    }
  }
}

void GymExerciseBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const std::string header =
      state_ == BODY_PART_LIST ? std::string(tr(STR_ADD_EXERCISE)) : capitalize(bodyParts_[static_cast<size_t>(bodyPartIndex_)]);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (state_ == BODY_PART_LIST) {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(bodyParts_.size()), bodyPartIndex_,
        [this](int i) { return capitalize(bodyParts_[static_cast<size_t>(i)]); }, nullptr, nullptr,
        [this](int i) {
          char buf[32];
          snprintf(buf, sizeof(buf), tr(STR_EXERCISE_COUNT_FORMAT), bodyPartCounts_[static_cast<size_t>(i)]);
          return std::string(buf);
        },
        false);
  } else if (filteredIndices_.empty()) {
    const int msgWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_NO_EXERCISES_FOR_BODY_PART));
    renderer.drawText(UI_10_FONT_ID, (pageWidth - msgWidth) / 2, contentTop + 40, tr(STR_NO_EXERCISES_FOR_BODY_PART));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(filteredIndices_.size()),
        exerciseIndex_,
        [this](int i) { return allExercises_[filteredIndices_[static_cast<size_t>(i)]].name; },
        [this](int i) { return allExercises_[filteredIndices_[static_cast<size_t>(i)]].equipment; }, nullptr, nullptr,
        false);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN),
                                            /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
