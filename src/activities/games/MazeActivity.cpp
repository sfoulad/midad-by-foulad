#include "MazeActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Out-of-line definitions for constexpr static arrays (C++14 ODR requirement)
constexpr int MazeActivity::SIZES_W[];
constexpr int MazeActivity::SIZES_H[];

// ---- Wall bit constants ----
// N=0x01, E=0x02, S=0x04, W=0x08
static constexpr uint8_t WALL_N = 0x01;
static constexpr uint8_t WALL_E = 0x02;
static constexpr uint8_t WALL_S = 0x04;
static constexpr uint8_t WALL_W = 0x08;
static constexpr uint8_t WALL_ALL = 0x0F;

// Direction table: dx, dy, wall-leaving, wall-entering-neighbor
static const int DIR_DX[4] = {0, 1, 0, -1};
static const int DIR_DY[4] = {-1, 0, 1, 0};
static const uint8_t DIR_WALL[4] = {WALL_N, WALL_E, WALL_S, WALL_W};
static const uint8_t DIR_OPP_WALL[4] = {WALL_S, WALL_W, WALL_N, WALL_E};

// ---- Helpers ----

void MazeActivity::fillDithered50(GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy++)
    for (int dx = (dy % 2); dx < w; dx += 2) r.drawPixel(x + dx, y + dy, true);
}

bool MazeActivity::isVisited(int x, int y) const {
  int idx = y * mazeW + x;
  return (visited[idx / 8] >> (idx % 8)) & 1;
}

void MazeActivity::setVisited(int x, int y) {
  int idx = y * mazeW + x;
  visited[idx / 8] |= static_cast<uint8_t>(1 << (idx % 8));
}

// ---- Maze generation (iterative recursive backtracker) ----

void MazeActivity::generateMaze() {
  // Initialize all cells with all 4 walls
  for (int y = 0; y < mazeH; y++)
    for (int x = 0; x < mazeW; x++) maze[y][x] = WALL_ALL;

  // Local bitfield for generation visited state
  uint8_t genVisited[(MAX_H * MAX_W) / 8 + 1];
  memset(genVisited, 0, sizeof(genVisited));

  // Stack of encoded cell indices (y * mazeW + x)
  int16_t stack[MAX_H * MAX_W];
  int stackTop = 0;

  // Start at (0, 0)
  int startIdx = 0;
  genVisited[startIdx / 8] |= static_cast<uint8_t>(1 << (startIdx % 8));
  stack[stackTop++] = static_cast<int16_t>(startIdx);

  while (stackTop > 0) {
    int16_t curIdx = stack[stackTop - 1];
    int cx = curIdx % mazeW;
    int cy = curIdx / mazeW;

    // Shuffle direction order
    int order[4] = {0, 1, 2, 3};
    // Fisher-Yates shuffle (4 elements)
    for (int i = 3; i > 0; i--) {
      int j = static_cast<int>(random(i + 1));
      int tmp = order[i];
      order[i] = order[j];
      order[j] = tmp;
    }

    bool carved = false;
    for (int di = 0; di < 4; di++) {
      int d = order[di];
      int nx = cx + DIR_DX[d];
      int ny = cy + DIR_DY[d];
      if (nx < 0 || nx >= mazeW || ny < 0 || ny >= mazeH) continue;
      int nIdx = ny * mazeW + nx;
      if ((genVisited[nIdx / 8] >> (nIdx % 8)) & 1) continue;

      // Carve: remove wall between current and neighbor
      maze[cy][cx] &= ~DIR_WALL[d];
      maze[ny][nx] &= ~DIR_OPP_WALL[d];

      genVisited[nIdx / 8] |= static_cast<uint8_t>(1 << (nIdx % 8));
      stack[stackTop++] = static_cast<int16_t>(nIdx);
      carved = true;
      break;
    }

    if (!carved) {
      stackTop--;
    }
  }

  playerX = 0;
  playerY = 0;
  exitX = mazeW - 1;
  exitY = mazeH - 1;
  moveCount = 0;
  startTime = millis();
}

// ---- Layout ----

// Y position of the separator line drawn below the "Moves / Time" info bar in
// render() -- shared by calculateLayout() so the grid's top edge is always
// computed from the header's REAL height instead of a guessed constant. The
// old hardcoded "+40" offset assumed a header+info-bar height that happened to
// fit the simulator's default (X4) metrics but was too short on X3 (wider
// portrait screen, same header/font metrics computed differently) -- the grid
// area then started above where the info text was actually drawn, so the top
// rows of the maze rendered underneath "Moves: N  Time: Ns" instead of below
// it. Must match render()'s own infoY/sepY math exactly.
int MazeActivity::infoBarBottom() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int infoY = metrics.topPadding + metrics.headerHeight + 6;
  return infoY + renderer.getTextHeight(SMALL_FONT_ID) + 4;
}

void MazeActivity::calculateLayout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = infoBarBottom() + metrics.verticalSpacing;
  int availW = renderer.getScreenWidth() - 20;
  int availH = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - 10;

  cellSize = std::min(availW / mazeW, availH / mazeH);
  if (cellSize < 4) cellSize = 4;
  if (cellSize > 30) cellSize = 30;

  int gridW = cellSize * mazeW;
  int gridH = cellSize * mazeH;
  offsetX = (renderer.getScreenWidth() - gridW) / 2;
  offsetY = contentTop + (availH - gridH) / 2;
}

// ---- BFS solver ----

void MazeActivity::startSolving() {
  state = SOLVING;
  solvePhase = PHASE_BFS;
  solvePathFound = false;
  solvePathLen = 0;
  solvePathDrawn = 0;

  memset(visited, 0, sizeof(visited));
  memset(parentIdx, -1, sizeof(parentIdx));

  solveQueueHead = 0;
  solveQueueTail = 0;
  lastSolveStepMs = 0;

  int startI = playerY * mazeW + playerX;
  solveQueue[solveQueueTail++] = static_cast<int16_t>(startI);
  setVisited(playerX, playerY);
  parentIdx[startI] = static_cast<int16_t>(startI);  // sentinel: start points to itself

  requestUpdate();
}

void MazeActivity::tracePath() {
  int exitI = exitY * mazeW + exitX;
  int startI = playerY * mazeW + playerX;

  // Walk parent chain from exit to start
  solvePathLen = 0;
  int cur = exitI;
  while (cur != startI && solvePathLen < MAX_PATH) {
    solvePath[solvePathLen++] = static_cast<int16_t>(cur);
    int par = parentIdx[cur];
    if (par < 0 || par == cur) break;
    cur = par;
  }
  // Bounds-checked: on the largest maze (40x60 = 2400 cells) the loop above
  // can legitimately fill solvePath to MAX_PATH before ever reaching
  // startI, and this final append was unconditional in the upstream
  // source -- writing past the end of the array into whatever member
  // follows it.
  if (solvePathLen < MAX_PATH) {
    solvePath[solvePathLen++] = static_cast<int16_t>(startI);
  }

  // Reverse so path goes from start to exit
  for (int i = 0, j = solvePathLen - 1; i < j; i++, j--) {
    int16_t tmp = solvePath[i];
    solvePath[i] = solvePath[j];
    solvePath[j] = tmp;
  }

  solvePathDrawn = 0;
  solvePathFound = true;
}

void MazeActivity::solveStep() {
  if (solvePhase == PHASE_BFS) {
    int exitI = exitY * mazeW + exitX;

    for (int batch = 0; batch < solveBatchSize; batch++) {
      if (solveQueueHead >= solveQueueTail) {
        // Queue exhausted — no path found
        state = SOLVE_DONE;
        requestUpdate();
        return;
      }

      int16_t curI = solveQueue[solveQueueHead++];
      int cx = curI % mazeW;
      int cy = curI / mazeW;

      if (curI == exitI) {
        tracePath();
        solvePhase = PHASE_TRACE_PATH;
        requestUpdate();
        return;
      }

      for (int d = 0; d < 4; d++) {
        if (maze[cy][cx] & DIR_WALL[d]) continue;  // wall blocks
        int nx = cx + DIR_DX[d];
        int ny = cy + DIR_DY[d];
        if (nx < 0 || nx >= mazeW || ny < 0 || ny >= mazeH) continue;
        if (isVisited(nx, ny)) continue;
        setVisited(nx, ny);
        int nI = ny * mazeW + nx;
        parentIdx[nI] = curI;
        solveQueue[solveQueueTail++] = static_cast<int16_t>(nI);
      }
    }

    requestUpdate();
  } else {
    // PHASE_TRACE_PATH: reveal path incrementally
    solvePathDrawn += 3;
    if (solvePathDrawn >= solvePathLen) {
      solvePathDrawn = solvePathLen;
      state = SOLVE_DONE;
    }
    requestUpdate();
  }
}

// ---- Drawing helpers ----

void MazeActivity::drawMaze() {
  // Outer border (double line)
  int gx = offsetX;
  int gy = offsetY;
  int gw = cellSize * mazeW;
  int gh = cellSize * mazeH;
  renderer.drawRect(gx, gy, gw, gh, true);
  renderer.drawRect(gx - 1, gy - 1, gw + 2, gh + 2, true);

  // Draw south and east walls for each cell
  for (int y = 0; y < mazeH; y++) {
    for (int x = 0; x < mazeW; x++) {
      int cx = offsetX + x * cellSize;
      int cy = offsetY + y * cellSize;

      // South wall
      if (maze[y][x] & WALL_S) {
        renderer.drawLine(cx, cy + cellSize, cx + cellSize, cy + cellSize, true);
      }
      // East wall
      if (maze[y][x] & WALL_E) {
        renderer.drawLine(cx + cellSize, cy, cx + cellSize, cy + cellSize, true);
      }
    }
  }

  // Draw top and left border walls explicitly for cells that have them
  for (int x = 0; x < mazeW; x++) {
    if (maze[0][x] & WALL_N) {
      int cx = offsetX + x * cellSize;
      renderer.drawLine(cx, offsetY, cx + cellSize, offsetY, true);
    }
  }
  for (int y = 0; y < mazeH; y++) {
    if (maze[y][0] & WALL_W) {
      int cy = offsetY + y * cellSize;
      renderer.drawLine(offsetX, cy, offsetX, cy + cellSize, true);
    }
  }
}

static void drawFilledCircle(GfxRenderer& r, int cx, int cy, int radius) {
  for (int dy = -radius; dy <= radius; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) dx++;
    if (dx > 0) {
      r.fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, true);
    } else {
      r.drawPixel(cx, cy + dy, true);
    }
  }
}

void MazeActivity::drawPlayer() {
  int cx = offsetX + playerX * cellSize + cellSize / 2;
  int cy = offsetY + playerY * cellSize + cellSize / 2;
  int r = cellSize / 3;
  if (r < 2) r = 2;
  drawFilledCircle(renderer, cx, cy, r);
}

void MazeActivity::drawExit() {
  int ex = offsetX + exitX * cellSize;
  int ey = offsetY + exitY * cellSize;
  fillDithered50(renderer, ex + 1, ey + 1, cellSize - 1, cellSize - 1);
  if (cellSize >= 10) {
    int tw = renderer.getTextWidth(SMALL_FONT_ID, "X");
    int th = renderer.getTextHeight(SMALL_FONT_ID);
    renderer.drawText(SMALL_FONT_ID, ex + (cellSize - tw) / 2, ey + (cellSize - th) / 2, "X", true);
  }
}

void MazeActivity::drawPaused() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Dim the frozen maze behind a centered panel, same treatment as
  // Snake/Tetris's pause screen.
  const int panelW = pageWidth * 3 / 4;
  const int panelH = 120;
  const int panelX = (pageWidth - panelW) / 2;
  const int panelY = (pageHeight - panelH) / 2;
  renderer.fillRect(panelX, panelY, panelW, panelH, false);
  renderer.drawRect(panelX, panelY, panelW, panelH);
  renderer.drawRect(panelX + 2, panelY + 2, panelW - 4, panelH - 4);

  renderer.drawCenteredText(UI_12_FONT_ID, panelY + 20, tr(STR_PAUSED), true, EpdFontFamily::BOLD);
}

// ---- Activity lifecycle ----

void MazeActivity::onEnter() {
  Activity::onEnter();
  state = SIZE_SELECT;
  sizeIndex = 1;
  requestUpdate();
}

void MazeActivity::onExit() { Activity::onExit(); }

void MazeActivity::loop() {
  if (state == SIZE_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    buttonNavigator_.onScrollNext([this] {
      sizeIndex = ButtonNavigator::nextIndex(sizeIndex, 3);
      requestUpdate();
    });
    buttonNavigator_.onScrollPrevious([this] {
      sizeIndex = ButtonNavigator::previousIndex(sizeIndex, 3);
      requestUpdate();
    });
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      mazeW = SIZES_W[sizeIndex];
      mazeH = SIZES_H[sizeIndex];
      generateMaze();
      calculateLayout();
      state = PLAYING;
      requestUpdate();
    }
    return;
  }

  if (state == PLAYING) {
    // Back pauses instead of exiting -- a maze can take a while to walk,
    // and an accidental Back shouldn't throw away that progress (same
    // reasoning as Snake/Tetris's pause screen).
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = PAUSED;
      pauseStartMs = millis();
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      startSolving();
      return;
    }

    // Movement - routed through ButtonNavigator's logical Up/Down/Left/
    // Right (same idiom as Snake/the My Books grid) instead of raw
    // MappedInputManager checks, firing on press for immediate feedback.
    buttonNavigator_.onPress({MappedInputManager::Button::Up}, [this] {
      if (playerY > 0 && !(maze[playerY][playerX] & WALL_N)) {
        playerY--;
        moveCount++;
        if (playerX == exitX && playerY == exitY) state = SOLVED_MSG;
        requestUpdate();
      }
    });
    buttonNavigator_.onPress({MappedInputManager::Button::Down}, [this] {
      if (playerY < mazeH - 1 && !(maze[playerY][playerX] & WALL_S)) {
        playerY++;
        moveCount++;
        if (playerX == exitX && playerY == exitY) state = SOLVED_MSG;
        requestUpdate();
      }
    });
    buttonNavigator_.onPress({MappedInputManager::Button::Left}, [this] {
      if (playerX > 0 && !(maze[playerY][playerX] & WALL_W)) {
        playerX--;
        moveCount++;
        if (playerX == exitX && playerY == exitY) state = SOLVED_MSG;
        requestUpdate();
      }
    });
    buttonNavigator_.onPress({MappedInputManager::Button::Right}, [this] {
      if (playerX < mazeW - 1 && !(maze[playerY][playerX] & WALL_E)) {
        playerX++;
        moveCount++;
        if (playerX == exitX && playerY == exitY) state = SOLVED_MSG;
        requestUpdate();
      }
    });
    return;
  }

  if (state == PAUSED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Resume: shift startTime forward by the paused duration so the
      // displayed elapsed time excludes time spent paused.
      startTime += millis() - pauseStartMs;
      state = PLAYING;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SOLVED_MSG) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      generateMaze();
      calculateLayout();
      state = PLAYING;
      requestUpdate();
    }
    return;
  }

  if (state == SOLVING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = PLAYING;
      requestUpdate();
      return;
    }
    unsigned long now = millis();
    if (now - lastSolveStepMs >= SOLVE_STEP_MS) {
      lastSolveStepMs = now;
      solveStep();
    }
    return;
  }

  if (state == SOLVE_DONE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      generateMaze();
      calculateLayout();
      state = PLAYING;
      requestUpdate();
    }
    return;
  }
}

// ---- Render ----

void MazeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MAZE));

  if (state == SIZE_SELECT) {
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, 3, sizeIndex,
        [](int i) -> std::string {
          static const char* names[] = {"Small (10x15)", "Medium (20x30)", "Large (40x60)"};
          return names[i];
        },
        [](int i) -> std::string {
          static const char* descs[] = {"Relaxing, ~30 moves", "Standard, ~120 moves", "Challenge, ~400 moves"};
          return descs[i];
        });

    // Size list is 2-way NavNext/NavPrevious (see buttonNavigator_.onNext/
    // onPrevious above), which the front Left/Right buttons drive as an
    // alternate path to the side Up/Down buttons -- label them "Up"/"Down"
    // to match what they actually do here, same convention GamesMenuActivity
    // uses for its own list.
    auto labels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_GENERATE), tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Info bar: moves + elapsed time. While paused, the elapsed time is
  // frozen at the moment Back was pressed rather than ticking live, since
  // the wall-clock time spent paused doesn't belong to the solve attempt.
  unsigned long elapsedMs = (state == PAUSED) ? (pauseStartMs - startTime) : (millis() - startTime);
  unsigned long elapsed = elapsedMs / 1000;
  char infoBuf[48];
  snprintf(infoBuf, sizeof(infoBuf), "Moves: %d   Time: %lus", moveCount, elapsed);
  int infoY = metrics.topPadding + metrics.headerHeight + 6;
  renderer.drawCenteredText(SMALL_FONT_ID, infoY, infoBuf, true);

  // Separator line below info bar -- must match infoBarBottom()'s own math
  // exactly (calculateLayout() uses it to size the grid area below this line).
  int sepY = infoBarBottom();
  renderer.drawLine(0, sepY, pageWidth, sepY, true);

  // Draw maze grid
  drawMaze();

  // Draw exit marker
  drawExit();

  // Draw BFS visited overlay (sparse dots, only during SOLVING/SOLVE_DONE)
  if (state == SOLVING || state == SOLVE_DONE) {
    int visitedCount = mazeW * mazeH;
    for (int idx = 0; idx < visitedCount; idx++) {
      int vx = idx % mazeW;
      int vy = idx / mazeW;
      if (!isVisited(vx, vy)) continue;
      int px = offsetX + vx * cellSize + cellSize / 2;
      int py = offsetY + vy * cellSize + cellSize / 2;
      renderer.drawPixel(px, py, true);
      if (cellSize >= 6) {
        renderer.drawPixel(px + 1, py, true);
        renderer.drawPixel(px, py + 1, true);
      }
    }

    // Draw solution path up to solvePathDrawn steps
    int drawLen = solvePathDrawn;
    if (drawLen > solvePathLen) drawLen = solvePathLen;
    for (int i = 0; i < drawLen; i++) {
      int si = solvePath[i];
      int sx = si % mazeW;
      int sy = si / mazeW;
      int pcx = offsetX + sx * cellSize + cellSize / 2;
      int pcy = offsetY + sy * cellSize + cellSize / 2;
      int pr = cellSize / 4;
      if (pr < 1) pr = 1;
      drawFilledCircle(renderer, pcx, pcy, pr);

      // Connecting line to next path step
      if (i + 1 < drawLen) {
        int ni = solvePath[i + 1];
        int nx2 = ni % mazeW;
        int ny2 = ni / mazeW;
        int ncx = offsetX + nx2 * cellSize + cellSize / 2;
        int ncy = offsetY + ny2 * cellSize + cellSize / 2;
        renderer.drawLine(pcx, pcy, ncx, ncy, true);
      }
    }
  }

  // Draw player (on top of path)
  drawPlayer();

  // Popup / pause overlays
  if (state == PAUSED) {
    drawPaused();
    auto labels = mappedInput.mapLabels(tr(STR_QUIT), tr(STR_RESUME), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SOLVED_MSG) {
    char winBuf[48];
    snprintf(winBuf, sizeof(winBuf), tr(STR_MAZE_SOLVED), moveCount);
    GUI.drawPopup(renderer, winBuf);
    auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_NEW_MAZE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SOLVE_DONE) {
    auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_NEW_MAZE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SOLVING) {
    auto labels = mappedInput.mapLabels(tr(STR_STOP), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // PLAYING -- Left/Right move the player horizontally (Up/Down move
    // vertically via the side buttons, which never get hint-bar labels in
    // this app -- same convention as Snake/Tetris/Sudoku).
    auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SOLVE), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
