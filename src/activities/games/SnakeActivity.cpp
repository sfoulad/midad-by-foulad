#include "SnakeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "GameHighScoresStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/GameInputDiagLog.h"

// 25% gray (light) — every other pixel in checkerboard on even rows only
static void fillDithered25(GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy += 2)
    for (int dx = ((dy / 2) % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

void SnakeActivity::onEnter() {
  Activity::onEnter();
  initGame();
}

void SnakeActivity::onExit() { Activity::onExit(); }

void SnakeActivity::initGame() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int screenW = renderer.getScreenWidth();
  int screenH = renderer.getScreenHeight();

  // Reserve top area for score and bottom for button hints
  int topReserve = metrics.topPadding + 25;
  int bottomReserve = metrics.buttonHintsHeight;

  gridW = screenW / CELL_SIZE;
  gridH = (screenH - topReserve - bottomReserve) / CELL_SIZE;
  offsetX = (screenW - gridW * CELL_SIZE) / 2;
  offsetY = topReserve;

  snake.clear();
  int startX = gridW / 2;
  int startY = gridH / 2;
  snake.push_back({startX, startY});
  snake.push_back({startX - 1, startY});
  snake.push_back({startX - 2, startY});

  dirX = 1;
  dirY = 0;
  nextDirX = 1;
  nextDirY = 0;
  score = 0;
  isNewBest = false;
  state = PLAYING;
  lastStepMs = millis();

  spawnFood();
  requestUpdate();
}

void SnakeActivity::spawnFood() {
  int attempts = 0;
  do {
    food.x = static_cast<int>(random(gridW));
    food.y = static_cast<int>(random(gridH));
    attempts++;
  } while (isSnakeAt(food.x, food.y) && attempts < 1000);
}

bool SnakeActivity::isSnakeAt(int x, int y) const {
  for (auto& seg : snake) {
    if (seg.x == x && seg.y == y) return true;
  }
  return false;
}

void SnakeActivity::step() {
  // Diagnostic: measure how long each frame's blocking render wait actually
  // takes, and log any that are abnormally slow. If the render task is
  // intermittently stalling, requestUpdateAndWait()'s bounded timeout (see
  // ActivityManager.cpp) lets the game keep going, but the ENTIRE stall
  // happens deep inside this single loop() call -- gpio.update() only runs
  // once per full main.cpp loop() iteration, at the top, never mid-iteration
  // -- so a full button press+release inside that window can be silently
  // missed. "Snake keeps moving but buttons do nothing" is exactly what that
  // looks like from the outside; this pins down whether it's really happening.
  const auto timedRequestUpdateAndWait = [this] {
    const unsigned long waitStartMs = millis();
    requestUpdateAndWait();
    const unsigned long waitMs = millis() - waitStartMs;
    if (waitMs > 500) {
      char buf[80];
      snprintf(buf, sizeof(buf), "%lu step render wait = %lu ms (slow/stalled)", millis(), waitMs);
      GameInputDiagLog::append(buf);
    }
  };

  // Apply buffered direction
  dirX = nextDirX;
  dirY = nextDirY;

  Point head = snake.front();
  Point newHead = {head.x + dirX, head.y + dirY};

  // Wall collision
  if (newHead.x < 0 || newHead.x >= gridW || newHead.y < 0 || newHead.y >= gridH) {
    state = GAME_OVER;
    isNewBest = GAME_SCORES.reportSnakeScore(static_cast<uint32_t>(score));
    timedRequestUpdateAndWait();
    return;
  }

  // Self collision
  if (isSnakeAt(newHead.x, newHead.y)) {
    state = GAME_OVER;
    isNewBest = GAME_SCORES.reportSnakeScore(static_cast<uint32_t>(score));
    timedRequestUpdateAndWait();
    return;
  }

  snake.insert(snake.begin(), newHead);

  // Check food
  if (newHead.x == food.x && newHead.y == food.y) {
    score += 10;
    spawnFood();
  } else {
    snake.pop_back();
  }

  // requestUpdateAndWait() (not the deferred requestUpdate()) is load-bearing
  // here, not just a style choice: rendering runs on a separate task woken by
  // a notification that coalesces if it arrives faster than the task can
  // service it (see ActivityManager::requestUpdate/renderTaskLoop). E-ink
  // full/fast-refresh cycles can take longer than STEP_INTERVAL_MS, and
  // loop()'s timer check is wall-clock-based with no catch-up throttle -- so
  // without blocking here, the snake's logical position could advance two or
  // more cells between two *physically shown* frames, which reads as the
  // snake teleporting/jumping instead of sliding one cell at a time (user
  // report). Blocking until this frame is actually on the panel makes the
  // effective step rate track real display speed instead of racing ahead of
  // it -- worst case the game paces itself to the panel's refresh time
  // instead of the nominal 300ms, which is the correct trade-off.
  timedRequestUpdateAndWait();
}

void SnakeActivity::loop() {
  // Buttons pressed at any point during the last blocking render wait (step()'s
  // requestUpdateAndWait(), ~800ms on this hardware -- see its own comment) that a
  // normal wasPressed() check here would otherwise never see: wasPressed()'s edge is
  // only valid for the specific gpio.update() call that detected it, and this
  // function's own checks run on a LATER call than the one buried inside that wait.
  // Confirmed on-device via diagnostic log: presses vanished with no trace across
  // whole play sessions. Treated as equivalent to a fresh wasPressed() below.
  const uint8_t waitPresses = consumePressesDuringLastWait();

  if (mappedInput.wasAnyPressed() || waitPresses != 0) {
    // Diagnostic: user reported buttons doing nothing in Snake while the snake
    // keeps auto-stepping (i.e. loop()/step() are clearly still running, so this
    // isn't the render-task hang fixed in requestUpdateAndWait()) -- unreproducible
    // off-device with no serial cable in hand. Log exactly what wasPressed() sees
    // on every real press so a report like that can still tell us whether input is
    // reaching this activity at all, and if so, why a turn/pause doesn't register.
    char buf[176];
    snprintf(buf, sizeof(buf),
             "%lu state=%d dir=(%d,%d) next=(%d,%d) U=%d D=%d L=%d R=%d Back=%d Confirm=%d waitPresses=0x%02x",
             millis(), static_cast<int>(state), dirX, dirY, nextDirX, nextDirY,
             mappedInput.wasPressed(MappedInputManager::Button::Up),
             mappedInput.wasPressed(MappedInputManager::Button::Down),
             mappedInput.wasPressed(MappedInputManager::Button::Left),
             mappedInput.wasPressed(MappedInputManager::Button::Right),
             mappedInput.wasPressed(MappedInputManager::Button::Back),
             mappedInput.wasPressed(MappedInputManager::Button::Confirm), waitPresses);
    GameInputDiagLog::append(buf);
  }

  const bool confirmPressed =
      mappedInput.wasPressed(MappedInputManager::Button::Confirm) || (waitPresses & ActivityManager::WAIT_BIT_CONFIRM);
  const bool backPressed =
      mappedInput.wasPressed(MappedInputManager::Button::Back) || (waitPresses & ActivityManager::WAIT_BIT_BACK);

  if (state == GAME_OVER) {
    if (confirmPressed) {
      initGame();
    }
    if (backPressed) {
      finish();
    }
    return;
  }

  if (state == PAUSED) {
    if (confirmPressed) {
      // Resume: re-baseline the step timer so the real time spent paused
      // doesn't register as one giant elapsed step (which would otherwise
      // make the snake jump forward multiple cells the instant play resumes).
      state = PLAYING;
      lastStepMs = millis();
      requestUpdate();
    }
    if (backPressed) {
      finish();
    }
    return;
  }

  // Direction input - prevent reversal. Routed through ButtonNavigator's
  // logical Up/Down/Left/Right (same idiom as the My Books grid) instead of
  // raw MappedInputManager checks, so remapped/orientation-swapped buttons
  // still turn the snake correctly. Fires on PRESS rather than release: a
  // release-gated turn only registers once the button comes back up, which
  // reads as input lag on real hardware -- a turn should land the instant
  // the button goes down.
  buttonNavigator_.onPress({MappedInputManager::Button::Up}, [this] {
    if (dirY == 0) {
      nextDirX = 0;
      nextDirY = -1;
    }
  });
  buttonNavigator_.onPress({MappedInputManager::Button::Down}, [this] {
    if (dirY == 0) {
      nextDirX = 0;
      nextDirY = 1;
    }
  });
  buttonNavigator_.onPress({MappedInputManager::Button::Left}, [this] {
    if (dirX == 0) {
      nextDirX = -1;
      nextDirY = 0;
    }
  });
  buttonNavigator_.onPress({MappedInputManager::Button::Right}, [this] {
    if (dirX == 0) {
      nextDirX = 1;
      nextDirY = 0;
    }
  });
  if (waitPresses & ActivityManager::WAIT_BIT_UP && dirY == 0) {
    nextDirX = 0;
    nextDirY = -1;
  } else if (waitPresses & ActivityManager::WAIT_BIT_DOWN && dirY == 0) {
    nextDirX = 0;
    nextDirY = 1;
  } else if (waitPresses & ActivityManager::WAIT_BIT_LEFT && dirX == 0) {
    nextDirX = -1;
    nextDirY = 0;
  } else if (waitPresses & ActivityManager::WAIT_BIT_RIGHT && dirX == 0) {
    nextDirX = 1;
    nextDirY = 0;
  }

  if (backPressed) {
    state = PAUSED;
    requestUpdate();
    return;
  }

  // Step timer
  unsigned long now = millis();
  if (now - lastStepMs >= STEP_INTERVAL_MS) {
    lastStepMs = now;
    step();
  }
}

void SnakeActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (state) {
    case PLAYING:
      renderPlaying();
      break;
    case PAUSED:
      renderPlaying();
      renderPaused();
      break;
    case GAME_OVER:
      renderGameOver();
      break;
  }

  renderer.displayBuffer();
}

void SnakeActivity::renderPlaying() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int screenW = renderer.getScreenWidth();

  // Header (score/length live here, in the reserved top band, so the bottom
  // button-hints bar never overlaps or covers them).
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, metrics.topPadding, "SNAKE", true,
                     EpdFontFamily::BOLD);
  char scoreBuf[64];
  snprintf(scoreBuf, sizeof(scoreBuf), "Score: %d  Length: %d  Best: %u", score, (int)snake.size(),
           GAME_SCORES.getSnakeHighScore());
  int scoreW = renderer.getTextWidth(UI_10_FONT_ID, scoreBuf, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, screenW - metrics.contentSidePadding - scoreW, metrics.topPadding, scoreBuf,
                     true, EpdFontFamily::BOLD);

  // Double-line border
  renderer.drawRect(offsetX - 1, offsetY - 1, gridW * CELL_SIZE + 2, gridH * CELL_SIZE + 2);
  renderer.drawRect(offsetX - 3, offsetY - 3, gridW * CELL_SIZE + 6, gridH * CELL_SIZE + 6);

  // Subtle background texture
  fillDithered25(renderer, offsetX, offsetY, gridW * CELL_SIZE, gridH * CELL_SIZE);

  // Snake body (with 1px white gap between segments for articulated look)
  for (size_t i = 0; i < snake.size(); i++) {
    int px = offsetX + snake[i].x * CELL_SIZE;
    int py = offsetY + snake[i].y * CELL_SIZE;
    if (i == 0) {
      // Head — filled with eyes
      renderer.fillRect(px + 1, py + 1, CELL_SIZE - 2, CELL_SIZE - 2);
      // Eyes based on direction
      if (dirX == 1) {  // right
        renderer.drawPixel(px + CELL_SIZE - 3, py + 2, false);
        renderer.drawPixel(px + CELL_SIZE - 3, py + CELL_SIZE - 3, false);
      } else if (dirX == -1) {  // left
        renderer.drawPixel(px + 2, py + 2, false);
        renderer.drawPixel(px + 2, py + CELL_SIZE - 3, false);
      } else if (dirY == -1) {  // up
        renderer.drawPixel(px + 2, py + 2, false);
        renderer.drawPixel(px + CELL_SIZE - 3, py + 2, false);
      } else {  // down
        renderer.drawPixel(px + 2, py + CELL_SIZE - 3, false);
        renderer.drawPixel(px + CELL_SIZE - 3, py + CELL_SIZE - 3, false);
      }
    } else {
      // Body segment with 1px gap (draw slightly smaller)
      renderer.fillRect(px + 2, py + 2, CELL_SIZE - 4, CELL_SIZE - 4);
    }
  }

  // Food — apple shape
  {
    int px = offsetX + food.x * CELL_SIZE;
    int py = offsetY + food.y * CELL_SIZE;
    int cx = px + CELL_SIZE / 2;
    int cy = py + CELL_SIZE / 2 + 1;
    int r = CELL_SIZE / 3;
    // Filled circle body
    for (int dy = -r; dy <= r; dy++) {
      int dx = 0;
      while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
      if (dx > 0)
        renderer.fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, true);
      else
        renderer.drawPixel(cx, cy + dy, true);
    }
    // Stem on top
    renderer.fillRect(cx, cy - r - 2, 2, 3, true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_PAUSE), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SnakeActivity::renderPaused() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Dim the frozen game board behind a centered panel so it's clear play is
  // suspended, not lost.
  const int panelW = pageWidth * 3 / 4;
  const int panelH = 120;
  const int panelX = (pageWidth - panelW) / 2;
  const int panelY = (pageHeight - panelH) / 2;
  renderer.fillRect(panelX, panelY, panelW, panelH, false);
  renderer.drawRect(panelX, panelY, panelW, panelH);
  renderer.drawRect(panelX + 2, panelY + 2, panelW - 4, panelH - 4);

  renderer.drawCenteredText(UI_12_FONT_ID, panelY + 20, tr(STR_PAUSED), true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_QUIT), tr(STR_RESUME), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SnakeActivity::renderGameOver() const {
  const auto pageHeight = renderer.getScreenHeight();
  int y = pageHeight / 2 - 40;

  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_GAME_OVER), true, EpdFontFamily::BOLD);
  y += 40;

  char scoreBuf[32];
  snprintf(scoreBuf, sizeof(scoreBuf), "Score: %d", score);
  renderer.drawCenteredText(UI_10_FONT_ID, y, scoreBuf);
  y += 22;

  char bestBuf[32];
  if (isNewBest) {
    snprintf(bestBuf, sizeof(bestBuf), "New Best!");
  } else {
    snprintf(bestBuf, sizeof(bestBuf), "Best: %u", GAME_SCORES.getSnakeHighScore());
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, bestBuf);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
