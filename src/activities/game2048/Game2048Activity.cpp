#include "Game2048Activity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Compacts one line of 4 cells toward index 0, merging equal adjacent values
// once per move (standard 2048 rules). Returns true if the line changed.
bool compactLine(int line[4], int& scoreDelta) {
  int original[4];
  for (int i = 0; i < 4; i++) original[i] = line[i];

  int vals[4];
  int n = 0;
  for (int i = 0; i < 4; i++) {
    if (line[i] != 0) vals[n++] = line[i];
  }

  int result[4] = {0, 0, 0, 0};
  int w = 0;
  for (int i = 0; i < n;) {
    if (i + 1 < n && vals[i] == vals[i + 1]) {
      result[w] = vals[i] * 2;
      scoreDelta += result[w];
      w++;
      i += 2;
    } else {
      result[w] = vals[i];
      w++;
      i++;
    }
  }

  bool changed = false;
  for (int i = 0; i < 4; i++) {
    line[i] = result[i];
    if (line[i] != original[i]) changed = true;
  }
  return changed;
}

}  // namespace

void Game2048Activity::onEnter() {
  Activity::onEnter();
  srand(millis());
  resetGame();
}

void Game2048Activity::onExit() { Activity::onExit(); }

void Game2048Activity::resetGame() {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      board[r][c] = 0;
    }
  }
  score = 0;
  gameOver = false;
  spawnRandomTile();
  spawnRandomTile();
}

void Game2048Activity::spawnRandomTile() {
  int emptyRows[16], emptyCols[16];
  int count = 0;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (board[r][c] == 0) {
        emptyRows[count] = r;
        emptyCols[count] = c;
        count++;
      }
    }
  }
  if (count == 0) return;
  int pick = rand() % count;
  board[emptyRows[pick]][emptyCols[pick]] = (rand() % 10 == 0) ? 4 : 2;
}

bool Game2048Activity::move(Game2048Direction direction) {
  bool changed = false;
  int scoreDelta = 0;

  if (direction == Game2048Direction::Left || direction == Game2048Direction::Right) {
    for (int r = 0; r < 4; r++) {
      int line[4];
      for (int i = 0; i < 4; i++) {
        int c = (direction == Game2048Direction::Left) ? i : 3 - i;
        line[i] = board[r][c];
      }
      if (compactLine(line, scoreDelta)) changed = true;
      for (int i = 0; i < 4; i++) {
        int c = (direction == Game2048Direction::Left) ? i : 3 - i;
        board[r][c] = line[i];
      }
    }
  } else {
    for (int c = 0; c < 4; c++) {
      int line[4];
      for (int i = 0; i < 4; i++) {
        int r = (direction == Game2048Direction::Up) ? i : 3 - i;
        line[i] = board[r][c];
      }
      if (compactLine(line, scoreDelta)) changed = true;
      for (int i = 0; i < 4; i++) {
        int r = (direction == Game2048Direction::Up) ? i : 3 - i;
        board[r][c] = line[i];
      }
    }
  }

  score += scoreDelta;
  return changed;
}

bool Game2048Activity::anyMovesLeft() const {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (board[r][c] == 0) return true;
      if (c + 1 < 4 && board[r][c] == board[r][c + 1]) return true;
      if (r + 1 < 4 && board[r][c] == board[r + 1][c]) return true;
    }
  }
  return false;
}

void Game2048Activity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (gameOver) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      resetGame();
      requestUpdate();
    }
    return;
  }

  Game2048Direction direction;
  bool pressed = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    direction = Game2048Direction::Left;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    direction = Game2048Direction::Right;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    direction = Game2048Direction::Up;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    direction = Game2048Direction::Down;
  } else {
    pressed = false;
  }

  if (pressed) {
    // A no-op move (nothing to compact toward that edge) skips the redraw:
    // e-ink refreshes are slow and visibly flicker, so don't spend one on a
    // move that didn't change anything.
    if (move(direction)) {
      spawnRandomTile();
      if (!anyMovesLeft()) gameOver = true;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    auto handler = [this](const ActivityResult& res) {
      if (!res.isCancelled) {
        resetGame();
      }
      requestUpdate();
    };
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_2048_RESTART_TITLE),
                                                tr(STR_2048_RESTART_BODY)),
        handler);
  }
}

void Game2048Activity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  int viewTop, viewRight, viewBottom, viewLeft;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);
  (void)viewTop;  // header chrome already clears the top bezel margin by a wide margin

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_2048_TITLE));

  // Score line
  char scoreBuf[32];
  snprintf(scoreBuf, sizeof(scoreBuf), tr(STR_2048_SCORE), score);
  const int scoreY = metrics.topPadding + metrics.headerHeight + 15;
  renderer.drawCenteredText(UI_12_FONT_ID, scoreY, scoreBuf, true, EpdFontFamily::BOLD);

  // Grid: square, centered, sized from available space (orientation-aware, no hardcoded pixels).
  const int gridTop = scoreY + 40;
  const int footerReserve = 60;  // status line + spacing above button hints
  const int availW = pageWidth - viewLeft - viewRight;
  const int availH = (pageHeight - viewBottom - metrics.buttonHintsHeight - footerReserve) - gridTop;
  const int cellSize = std::min(availW, availH) / 4;
  const int gridSize = cellSize * 4;
  const int gridX = (pageWidth - gridSize) / 2;
  const int gridY = gridTop;

  renderer.drawRect(gridX, gridY, gridSize, gridSize, 2, true);
  for (int i = 1; i < 4; i++) {
    renderer.drawLine(gridX + i * cellSize, gridY, gridX + i * cellSize, gridY + gridSize, 2, true);
    renderer.drawLine(gridX, gridY + i * cellSize, gridX + gridSize, gridY + i * cellSize, 2, true);
  }

  const int pad = cellSize / 10;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      int value = board[r][c];
      if (value == 0) continue;

      int cx = gridX + c * cellSize;
      int cy = gridY + r * cellSize;
      renderer.drawRoundedRect(cx + pad, cy + pad, cellSize - 2 * pad, cellSize - 2 * pad, 2, 6, true);

      char valBuf[8];
      snprintf(valBuf, sizeof(valBuf), "%d", value);
      int tW = renderer.getTextWidth(NOTOSANS_18_FONT_ID, valBuf, EpdFontFamily::BOLD);
      int tH = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
      renderer.drawText(NOTOSANS_18_FONT_ID, cx + (cellSize - tW) / 2, cy + (cellSize - tH) / 2, valBuf, true,
                        EpdFontFamily::BOLD);
    }
  }

  // Footer status text
  const int footerY = gridY + gridSize + 20;
  if (gameOver) {
    char overBuf[48];
    snprintf(overBuf, sizeof(overBuf), tr(STR_2048_GAME_OVER), score);
    renderer.drawCenteredText(UI_12_FONT_ID, footerY, overBuf, true, EpdFontFamily::REGULAR);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_2048_NEW_GAME), nullptr, nullptr);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
