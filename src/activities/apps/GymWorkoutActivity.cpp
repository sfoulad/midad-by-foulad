#include "GymWorkoutActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "GymCatalog.h"
#include "GymLogStore.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/GymDiagLog.h"

namespace {
constexpr float KG_TO_LB = 2.20462f;
constexpr int kImageMaxWidth = 400;
constexpr int kImageMaxHeight = 280;
constexpr int kDotSize = 14;
constexpr int kDotGap = 10;

std::string formatWeight(const float weightKg) {
  const bool isLb = SETTINGS.gymWeightUnit == CrossPointSettings::GYM_WEIGHT_LB;
  const float displayValue = isLb ? weightKg * KG_TO_LB : weightKg;
  char buf[24];
  snprintf(buf, sizeof(buf), "%.1f %s", static_cast<double>(displayValue), isLb ? tr(STR_LB_SHORT) : tr(STR_KG_SHORT));
  return buf;
}
}  // namespace

GymWorkoutActivity::GymWorkoutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const size_t dayIndex)
    : Activity("GymWorkout", renderer, mappedInput), dayIndex_(dayIndex) {}

const PlannedExercise* GymWorkoutActivity::currentExercise() const {
  const auto& day = GYM_PLAN.getDay(dayIndex_);
  if (exerciseIndex_ >= day.exercises.size()) return nullptr;
  return &day.exercises[exerciseIndex_];
}

void GymWorkoutActivity::beginExercise() {
  currentSet_ = 1;
  const auto* ex = currentExercise();
  if (!ex) return;

  // ensureImageThumb generates (and caches to SD) a 1-bit dithered BMP the
  // first time this exercise's photo is needed -- see its header comment for
  // why that's a real BW-mode-safe dither, unlike the JPEG decoder's own
  // 4-level dither. Treat generation failure as imageless rather than
  // crashing or showing a broken image.
  hasImage_ = Storage.exists(GymCatalog::imagePath(ex->slug).c_str()) &&
              GymCatalog::ensureImageThumb(ex->slug, kImageMaxWidth, kImageMaxHeight);

  if (const auto* last = GYM_LOG.findPerformance(ex->slug)) {
    weightKg_ = last->lastWeightKg;
    reps_ = last->lastReps;
  } else {
    weightKg_ = 0.0f;
    reps_ = static_cast<int>(ex->targetReps);
  }
}

void GymWorkoutActivity::onEnter() {
  Activity::onEnter();
  const auto& day = GYM_PLAN.getDay(dayIndex_);
  if (day.exercises.empty()) {
    finish();
    return;
  }
  exerciseIndex_ = 0;
  state_ = EXERCISING;
  sessionStartMs_ = millis();
  setsLoggedThisSession_ = 0;
  beginExercise();
  requestUpdate();

  char buf[64];
  snprintf(buf, sizeof(buf), "%lu gym workout_start day=%d exercises=%d", millis(), static_cast<int>(dayIndex_) + 1,
           static_cast<int>(day.exercises.size()));
  GymDiagLog::append(buf);
}

void GymWorkoutActivity::logSetAndAdvance() {
  const auto* ex = currentExercise();
  if (!ex) return;
  GYM_LOG.recordSet(ex->slug, weightKg_, static_cast<uint8_t>(std::clamp(reps_, 0, 255)));
  ++setsLoggedThisSession_;

  const auto& day = GYM_PLAN.getDay(dayIndex_);
  if (static_cast<uint8_t>(currentSet_) < ex->targetSets) {
    ++currentSet_;
  } else {
    ++exerciseIndex_;
    if (exerciseIndex_ >= day.exercises.size()) {
      finishWorkoutComplete();
      return;
    }
    beginExercise();
  }
  requestUpdate();
}

void GymWorkoutActivity::finishWorkoutComplete() {
  GYM_LOG.recordSessionCompleted();
  GYM_PLAN.advanceToNextDay();
  state_ = COMPLETE;
  requestUpdate();

  char buf[96];
  snprintf(buf, sizeof(buf), "%lu gym workout_complete day=%d sets=%d elapsed=%lums", millis(),
           static_cast<int>(dayIndex_) + 1, setsLoggedThisSession_, millis() - sessionStartMs_);
  GymDiagLog::append(buf);
}

void GymWorkoutActivity::promptEndWorkout() {
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_END_WORKOUT_TITLE),
                                                                tr(STR_END_WORKOUT_BODY)),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             GYM_LOG.saveToFile();
                             char buf[96];
                             snprintf(buf, sizeof(buf), "%lu gym workout_ended_early day=%d sets=%d elapsed=%lums",
                                      millis(), static_cast<int>(dayIndex_) + 1, setsLoggedThisSession_,
                                      millis() - sessionStartMs_);
                             GymDiagLog::append(buf);
                             finish();
                             return;
                           }
                           requestUpdate();
                         });
}

void GymWorkoutActivity::loop() {
  if (state_ == COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      GYM_LOG.saveToFile();
      finish();
    }
    return;
  }

  // wasReleased, not wasPressed -- matching GymActivity's own Back-exit and
  // this same screen's COMPLETE-state handling below. Mixing press-edge and
  // release-edge Back checks across the Gym/GymDayDetail/GymWorkout stack
  // made a fast double-tap (encouraged by e-ink's slow ~500ms+ visual
  // feedback, which makes a first tap look like it didn't register) land on
  // whichever activity happened to be current when each edge fired, cascading
  // an extra unintended pop (confirmed via on-device serial log).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    promptEndWorkout();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    weightKg_ = std::max(0.0f, weightKg_ + WEIGHT_STEP_KG);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    weightKg_ = std::max(0.0f, weightKg_ - WEIGHT_STEP_KG);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    logSetAndAdvance();
  }
}

void GymWorkoutActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (state_ == COMPLETE) {
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GYM));
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int msgWidth = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_WORKOUT_COMPLETE), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, (pageWidth - msgWidth) / 2, (pageHeight - lineHeight) / 2,
                      tr(STR_WORKOUT_COMPLETE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_DONE), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto* ex = currentExercise();
  if (!ex) {
    renderer.clearScreen();
    renderer.displayBuffer();
    return;
  }

  // The image (when present) is a pre-dithered 1-bit BMP -- see
  // GymCatalog::ensureImageThumb -- so drawing it is just a bitmap blit, as
  // cheap as any other element on this screen. No multi-pass grayscale
  // sequence, no one-time-vs-incremental split needed: every render() call
  // redraws the whole screen in one plain BW pass, same as every other
  // activity in this codebase.
  renderer.clearScreen();
  drawExercisingScreen(*ex);
  renderer.displayBuffer();
}

void GymWorkoutActivity::drawExercisingScreen(const PlannedExercise& ex) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, ex.name.c_str());

  int contentY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 8;

  char setBuf[24];
  snprintf(setBuf, sizeof(setBuf), tr(STR_SET_FORMAT), currentSet_, ex.targetSets);
  const int setWidth = renderer.getTextWidth(UI_10_FONT_ID, setBuf);
  renderer.drawText(UI_10_FONT_ID, (pageWidth - setWidth) / 2, contentY, setBuf);
  contentY += renderer.getLineHeight(UI_10_FONT_ID) + 10;

  // Set-progress dots: one per target set, filled for sets already logged,
  // the current set outlined+filled, remaining ones outline-only. More
  // scannable at a glance than the "Set X/Y" text alone.
  {
    const int totalSets = ex.targetSets;
    const int rowWidth = totalSets * kDotSize + (totalSets - 1) * kDotGap;
    int dotX = (pageWidth - rowWidth) / 2;
    for (int s = 1; s <= totalSets; ++s) {
      if (s < currentSet_) {
        renderer.fillRoundedRect(dotX, contentY, kDotSize, kDotSize, kDotSize / 2, Color::Black);
      } else if (s == currentSet_) {
        renderer.fillRoundedRect(dotX, contentY, kDotSize, kDotSize, kDotSize / 2, Color::Black);
        renderer.drawRoundedRect(dotX - 2, contentY - 2, kDotSize + 4, kDotSize + 4, 1, kDotSize / 2 + 2, true);
      } else {
        renderer.drawRoundedRect(dotX, contentY, kDotSize, kDotSize, 1, kDotSize / 2, true);
      }
      dotX += kDotSize + kDotGap;
    }
    contentY += kDotSize + 14;
  }

  if (hasImage_) {
    HalFile thumbFile;
    if (Storage.openFileForRead("GYM", GymCatalog::imageThumbPath(ex.slug, kImageMaxWidth, kImageMaxHeight).c_str(),
                                thumbFile)) {
      Bitmap bitmap(thumbFile);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const int imgX = (pageWidth - kImageMaxWidth) / 2;
        renderer.drawBitmap(bitmap, imgX, contentY, kImageMaxWidth, kImageMaxHeight);
      }
    }
    contentY += kImageMaxHeight + 16;
  }

  const std::string weightStr = formatWeight(weightKg_);
  char bigBuf[40];
  snprintf(bigBuf, sizeof(bigBuf), "%s x %d", weightStr.c_str(), reps_);
  const int bigWidth = renderer.getTextWidth(UI_12_FONT_ID, bigBuf, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, (pageWidth - bigWidth) / 2, contentY, bigBuf, true, EpdFontFamily::BOLD);
  contentY += renderer.getLineHeight(UI_12_FONT_ID) + 8;

  if (const auto* last = GYM_LOG.findPerformance(ex.slug)) {
    char lastBuf[48];
    snprintf(lastBuf, sizeof(lastBuf), tr(STR_LAST_TIME_FORMAT), formatWeight(last->lastWeightKg).c_str(),
             last->lastReps);
    const int lastWidth = renderer.getTextWidth(SMALL_FONT_ID, lastBuf);
    renderer.drawText(SMALL_FONT_ID, (pageWidth - lastWidth) / 2, contentY, lastBuf);
  }

  // Weight is adjusted via the physical side buttons -- symbols only ("+"/"-"),
  // language-neutral so no i18n string is needed. Front Left/Right no longer
  // adjust reps (removed per user request) -- reps_ is still tracked (shown
  // here, logged with the set) but is no longer live-editable mid-workout.
  GUI.drawSideButtonHints(renderer, "+", "-");
  const auto labels = mappedInput.mapLabels(tr(STR_END), tr(STR_LOG_SET), "", "", /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
