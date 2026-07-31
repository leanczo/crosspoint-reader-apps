#include "TicTacToeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// 3 rows + 3 cols + 2 diagonals, each as {row, col} pairs.
static constexpr int LINES[8][3][2] = {
    {{0, 0}, {0, 1}, {0, 2}}, {{1, 0}, {1, 1}, {1, 2}}, {{2, 0}, {2, 1}, {2, 2}},  // rows
    {{0, 0}, {1, 0}, {2, 0}}, {{0, 1}, {1, 1}, {2, 1}}, {{0, 2}, {1, 2}, {2, 2}},  // cols
    {{0, 0}, {1, 1}, {2, 2}}, {{0, 2}, {1, 1}, {2, 0}},                           // diagonals
};
}  // namespace

void TicTacToeActivity::onEnter() {
  Activity::onEnter();
  resetGame();
}

void TicTacToeActivity::onExit() { Activity::onExit(); }

void TicTacToeActivity::resetGame() {
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      board[r][c] = 0;
    }
  }
  cursorRow = 1;
  cursorCol = 1;
  xTurn = true;
  gameOver = false;
  isDraw = false;
  winner = 0;
  winStartRow = winStartCol = winEndRow = winEndCol = -1;
}

bool TicTacToeActivity::findWinner() {
  for (const auto& line : LINES) {
    int a = board[line[0][0]][line[0][1]];
    int b = board[line[1][0]][line[1][1]];
    int c = board[line[2][0]][line[2][1]];
    if (a != 0 && a == b && b == c) {
      winner = a;
      winStartRow = line[0][0];
      winStartCol = line[0][1];
      winEndRow = line[2][0];
      winEndCol = line[2][1];
      return true;
    }
  }
  return false;
}

bool TicTacToeActivity::boardFull() const {
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      if (board[r][c] == 0) return false;
    }
  }
  return true;
}

void TicTacToeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (cursorRow == 0) {
      cursorRow = -1;
      cursorCol = 0;  // Highlight "New Game"
    } else if (cursorRow == -1) {
      cursorRow = 2;
    } else {
      cursorRow--;
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (cursorRow == 2) {
      cursorRow = -1;
      cursorCol = 0;  // Highlight "New Game"
    } else if (cursorRow == -1) {
      cursorRow = 0;
    } else {
      cursorRow++;
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (cursorRow != -1) {
      cursorCol = (cursorCol - 1 + 3) % 3;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (cursorRow != -1) {
      cursorCol = (cursorCol + 1) % 3;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (cursorRow == -1) {
      resetGame();
      requestUpdate();
    } else if (!gameOver && board[cursorRow][cursorCol] == 0) {
      board[cursorRow][cursorCol] = xTurn ? 1 : 2;
      if (findWinner()) {
        gameOver = true;
      } else if (boardFull()) {
        isDraw = true;
        gameOver = true;
      } else {
        xTurn = !xTurn;
      }
      requestUpdate();
    }
  }
}

void TicTacToeActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  int viewTop, viewRight, viewBottom, viewLeft;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);
  (void)viewTop;  // header chrome already clears the top bezel margin by a wide margin

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TICTACTOE_TITLE));

  // Toolbar: single "New Game" pill, reached via cursorRow == -1
  const int toolbarY = metrics.topPadding + metrics.headerHeight + 20;
  const int toolbarBtnW = 120;
  const int toolbarBtnH = 30;
  const int toolbarBtnX = (pageWidth - toolbarBtnW) / 2;
  bool newGameSelected = (cursorRow == -1);
  renderer.drawRoundedRect(toolbarBtnX, toolbarY, toolbarBtnW, toolbarBtnH, 1, 6, true);
  if (newGameSelected) {
    renderer.fillRoundedRect(toolbarBtnX, toolbarY, toolbarBtnW, toolbarBtnH, 6, Color::Black);
  }
  int ngTextW = renderer.getTextWidth(SMALL_FONT_ID, tr(STR_TICTACTOE_NEW_GAME));
  int ngTextX = toolbarBtnX + (toolbarBtnW - ngTextW) / 2;
  int ngTextY = toolbarY + (toolbarBtnH - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
  renderer.drawText(SMALL_FONT_ID, ngTextX, ngTextY, tr(STR_TICTACTOE_NEW_GAME), !newGameSelected);

  // Grid: square, centered, sized from available space (orientation-aware, no hardcoded pixels).
  // getOrientedViewableTRBL() returns bezel MARGINS (amount to inset from each edge), not
  // absolute coordinates, so the usable width/height is screen size minus both margins.
  const int gridTop = toolbarY + toolbarBtnH + 30;
  const int footerReserve = 70;  // status line + spacing above button hints
  const int availW = pageWidth - viewLeft - viewRight;
  const int availH = (pageHeight - viewBottom - metrics.buttonHintsHeight - footerReserve) - gridTop;
  const int cellSize = std::min(availW, availH) / 3;
  const int gridSize = cellSize * 3;
  const int gridX = (pageWidth - gridSize) / 2;
  const int gridY = gridTop;

  // Grid lines
  renderer.drawRect(gridX, gridY, gridSize, gridSize, 2, true);
  for (int i = 1; i < 3; i++) {
    renderer.drawLine(gridX + i * cellSize, gridY, gridX + i * cellSize, gridY + gridSize, 3, true);
    renderer.drawLine(gridX, gridY + i * cellSize, gridX + gridSize, gridY + i * cellSize, 3, true);
  }

  // Marks + cursor highlight
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      int cx = gridX + c * cellSize;
      int cy = gridY + r * cellSize;

      if (r == cursorRow && c == cursorCol && !gameOver && board[r][c] == 0) {
        renderer.drawRect(cx + 4, cy + 4, cellSize - 8, cellSize - 8, 2, true);
      }

      int pad = cellSize / 5;
      if (board[r][c] == 1) {
        // X: two thick diagonal strokes
        renderer.drawLine(cx + pad, cy + pad, cx + cellSize - pad, cy + cellSize - pad, 5, true);
        renderer.drawLine(cx + cellSize - pad, cy + pad, cx + pad, cy + cellSize - pad, 5, true);
      } else if (board[r][c] == 2) {
        // O: hollow ring via a max-corner-radius rounded rect
        int side = cellSize - 2 * pad;
        renderer.drawRoundedRect(cx + pad, cy + pad, side, side, 5, side / 2, true);
      }
    }
  }

  // Winning strikethrough — static flourish, drawn once after game over
  if (gameOver && winner != 0) {
    int sx = gridX + winStartCol * cellSize + cellSize / 2;
    int sy = gridY + winStartRow * cellSize + cellSize / 2;
    int ex = gridX + winEndCol * cellSize + cellSize / 2;
    int ey = gridY + winEndRow * cellSize + cellSize / 2;
    renderer.drawLine(sx, sy, ex, ey, 4, true);
  }

  // Footer status text
  const int footerY = gridY + gridSize + 20;
  const char* status;
  if (gameOver && winner != 0) {
    status = (winner == 1) ? tr(STR_TICTACTOE_X_WINS) : tr(STR_TICTACTOE_O_WINS);
  } else if (isDraw) {
    status = tr(STR_TICTACTOE_DRAW);
  } else {
    status = xTurn ? tr(STR_TICTACTOE_X_TURN) : tr(STR_TICTACTOE_O_TURN);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, footerY, status, true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
