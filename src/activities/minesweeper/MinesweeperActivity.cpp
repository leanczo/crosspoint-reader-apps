#include "MinesweeperActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdlib>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void MinesweeperActivity::computeGrid() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const int contentTop = metrics.topPadding + metrics.headerHeight + SUBHEADER_HEIGHT + metrics.verticalSpacing +
                          TOOLBAR_HEIGHT + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  cols = pageWidth / CELL_SIZE;
  rows = contentHeight / CELL_SIZE;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  if (cols * rows > MAX_CELLS) {
    // Defensive only: CELL_SIZE keeps cols*rows well under MAX_CELLS on this
    // display, but never let a future metrics change overflow the fixed arrays.
    rows = MAX_CELLS / cols;
    if (rows < 1) rows = 1;
  }

  const int gridPixelW = cols * CELL_SIZE;
  const int gridPixelH = rows * CELL_SIZE;
  gridOriginX = (pageWidth - gridPixelW) / 2;
  gridOriginY = contentTop + (contentHeight - gridPixelH) / 2;
}

void MinesweeperActivity::resetGame() {
  computeGrid();
  const int totalCells = cols * rows;
  for (int i = 0; i < totalCells; i++) {
    mine[i] = false;
    revealed[i] = false;
    flagged[i] = false;
    adjacentCount[i] = 0;
  }

  // Guard against a degenerate near-zero-cell grid (should not happen on the
  // real 800x480 panel, but never let placeMines() loop forever hunting for
  // mine slots that can't exist).
  mineCount = 0;
  if (totalCells >= 2) {
    mineCount = static_cast<int>(totalCells * MINE_DENSITY + 0.5f);
    if (mineCount < 1) mineCount = 1;
    if (mineCount > totalCells - 1) mineCount = totalCells - 1;
  }

  cursorRow = 0;
  cursorCol = cols / 2;
  flagMode = false;
  firstReveal = true;
  gameOver = false;
  won = false;
  flaggedCount = 0;
  revealedSafeCount = 0;
}

void MinesweeperActivity::placeMines(int excludeIndex) {
  const int totalCells = cols * rows;
  int placed = 0;
  while (placed < mineCount) {
    const int idx = rand() % totalCells;
    if (idx == excludeIndex || mine[idx]) continue;
    mine[idx] = true;
    placed++;
  }

  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const int idx = r * cols + c;
      if (mine[idx]) continue;
      int count = 0;
      for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
          if (dr == 0 && dc == 0) continue;
          const int nr = r + dr;
          const int nc = c + dc;
          if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
          if (mine[nr * cols + nc]) count++;
        }
      }
      adjacentCount[idx] = static_cast<int8_t>(count);
    }
  }
}

void MinesweeperActivity::revealCell(int row, int col) {
  const int startIdx = row * cols + col;
  if (flagged[startIdx] || revealed[startIdx]) return;

  // Iterative flood-fill using the fixed member stack (never a large local —
  // see Stack Safety rule) so zero-adjacency cells cascade open like classic
  // Minesweeper without risking a deep call stack on this constrained target.
  int top = 0;
  floodStack[top++] = startIdx;

  while (top > 0) {
    const int idx = floodStack[--top];
    if (revealed[idx] || flagged[idx]) continue;
    revealed[idx] = true;

    if (mine[idx]) {
      gameOver = true;
      return;
    }
    revealedSafeCount++;

    if (adjacentCount[idx] == 0) {
      const int r = idx / cols;
      const int c = idx % cols;
      for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
          if (dr == 0 && dc == 0) continue;
          const int nr = r + dr;
          const int nc = c + dc;
          if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
          const int nIdx = nr * cols + nc;
          if (!revealed[nIdx] && !flagged[nIdx] && top < MAX_CELLS) {
            floodStack[top++] = nIdx;
          }
        }
      }
    }
  }
}

void MinesweeperActivity::toggleFlag(int row, int col) {
  const int idx = row * cols + col;
  if (revealed[idx]) return;
  flagged[idx] = !flagged[idx];
  flaggedCount += flagged[idx] ? 1 : -1;
}

void MinesweeperActivity::checkWin() {
  if (revealedSafeCount >= cols * rows - mineCount) {
    won = true;
  }
}

void MinesweeperActivity::onEnter() {
  Activity::onEnter();
  srand(millis());
  resetGame();
  requestUpdate();
}

void MinesweeperActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (gameOver || won) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      resetGame();
      requestUpdate();
    }
    return;
  }

  if (cursorRow == -1) {
    // Toolbar: two pills, "New Game" (col 0) and the Reveal/Flag mode toggle (col 1).
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      cursorCol = (cursorCol == 0) ? 1 : 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      cursorRow = 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (cursorCol == 0) {
        resetGame();
      } else {
        flagMode = !flagMode;
      }
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    cursorRow = (cursorRow == 0) ? -1 : cursorRow - 1;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    cursorRow = (cursorRow == rows - 1) ? -1 : cursorRow + 1;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (cursorCol > 0) {
      cursorCol--;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (cursorCol < cols - 1) {
      cursorCol++;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int idx = cursorRow * cols + cursorCol;
    if (flagMode) {
      toggleFlag(cursorRow, cursorCol);
    } else if (!flagged[idx]) {
      if (firstReveal) {
        firstReveal = false;
        placeMines(idx);
      }
      revealCell(cursorRow, cursorCol);
      if (!gameOver) {
        checkWin();
      }
    }
    requestUpdate();
  }
}

void MinesweeperActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MINESWEEPER_TITLE));

  const int remainingMines = mineCount - flaggedCount;
  char minesBuf[32];
  snprintf(minesBuf, sizeof(minesBuf), "%s %d", tr(STR_MINESWEEPER_MINES_LABEL), remainingMines);
  const int subHeaderY = metrics.topPadding + metrics.headerHeight;
  GUI.drawSubHeader(renderer, Rect{0, subHeaderY, pageWidth, SUBHEADER_HEIGHT}, minesBuf, nullptr);

  // Toolbar: two pills, "New Game" and the Reveal/Flag mode toggle.
  const int toolbarY = subHeaderY + SUBHEADER_HEIGHT + metrics.verticalSpacing;
  const int toolbarBtnW = 140;
  const int toolbarBtnH = TOOLBAR_HEIGHT;
  const int toolbarGap = 20;
  const int toolbarTotalW = toolbarBtnW * 2 + toolbarGap;
  const int toolbarStartX = (pageWidth - toolbarTotalW) / 2;

  const bool onToolbar = (cursorRow == -1);
  const bool newGameSelected = onToolbar && cursorCol == 0;
  const bool modeSelected = onToolbar && cursorCol == 1;

  const int newGameX = toolbarStartX;
  renderer.drawRoundedRect(newGameX, toolbarY, toolbarBtnW, toolbarBtnH, 1, 6, true);
  if (newGameSelected) {
    renderer.fillRoundedRect(newGameX, toolbarY, toolbarBtnW, toolbarBtnH, 6, Color::Black);
  }
  {
    const char* label = tr(STR_MINESWEEPER_NEW_GAME);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, label);
    const int textX = newGameX + (toolbarBtnW - textW) / 2;
    const int textY = toolbarY + (toolbarBtnH - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, textX, textY, label, !newGameSelected);
  }

  const int modeX = newGameX + toolbarBtnW + toolbarGap;
  renderer.drawRoundedRect(modeX, toolbarY, toolbarBtnW, toolbarBtnH, 1, 6, true);
  if (modeSelected) {
    renderer.fillRoundedRect(modeX, toolbarY, toolbarBtnW, toolbarBtnH, 6, Color::Black);
  }
  {
    const char* label = flagMode ? tr(STR_MINESWEEPER_MODE_FLAG) : tr(STR_MINESWEEPER_MODE_REVEAL);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, label);
    const int textX = modeX + (toolbarBtnW - textW) / 2;
    const int textY = toolbarY + (toolbarBtnH - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, textX, textY, label, !modeSelected);
  }

  // Grid border + internal lines
  renderer.drawRect(gridOriginX, gridOriginY, cols * CELL_SIZE, rows * CELL_SIZE, 2, true);
  for (int i = 1; i < cols; i++) {
    renderer.drawLine(gridOriginX + i * CELL_SIZE, gridOriginY, gridOriginX + i * CELL_SIZE,
                       gridOriginY + rows * CELL_SIZE, 1, true);
  }
  for (int i = 1; i < rows; i++) {
    renderer.drawLine(gridOriginX, gridOriginY + i * CELL_SIZE, gridOriginX + cols * CELL_SIZE,
                       gridOriginY + i * CELL_SIZE, 1, true);
  }

  // Cell contents + cursor
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const int idx = r * cols + c;
      const int cx = gridOriginX + c * CELL_SIZE;
      const int cy = gridOriginY + r * CELL_SIZE;

      if (!gameOver && !won && r == cursorRow && c == cursorCol) {
        renderer.drawRect(cx + 3, cy + 3, CELL_SIZE - 6, CELL_SIZE - 6, 2, true);
      }

      if (flagged[idx]) {
        const char* label = "F";
        const int textW = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
        const int textH = renderer.getLineHeight(UI_12_FONT_ID);
        renderer.drawText(UI_12_FONT_ID, cx + (CELL_SIZE - textW) / 2, cy + (CELL_SIZE - textH) / 2, label, true,
                           EpdFontFamily::BOLD);
      } else if (revealed[idx]) {
        if (mine[idx]) {
          const int pad = CELL_SIZE / 4;
          renderer.fillRect(cx + pad, cy + pad, CELL_SIZE - 2 * pad, CELL_SIZE - 2 * pad, true);
        } else if (adjacentCount[idx] > 0) {
          char buf[2] = {static_cast<char>('0' + adjacentCount[idx]), '\0'};
          const int textW = renderer.getTextWidth(UI_12_FONT_ID, buf, EpdFontFamily::BOLD);
          const int textH = renderer.getLineHeight(UI_12_FONT_ID);
          renderer.drawText(UI_12_FONT_ID, cx + (CELL_SIZE - textW) / 2, cy + (CELL_SIZE - textH) / 2, buf, true,
                             EpdFontFamily::BOLD);
        }
      } else if (gameOver && mine[idx]) {
        const int pad = CELL_SIZE / 4;
        renderer.fillRect(cx + pad, cy + pad, CELL_SIZE - 2 * pad, CELL_SIZE - 2 * pad, true);
      }
    }
  }

  if (gameOver) {
    GUI.drawPopup(renderer, tr(STR_MINESWEEPER_BOOM));
  } else if (won) {
    GUI.drawPopup(renderer, tr(STR_MINESWEEPER_WIN));
  }

  const char* confirmLabel = (gameOver || won) ? tr(STR_MINESWEEPER_NEW_GAME) : nullptr;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, nullptr, nullptr);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
