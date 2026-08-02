#pragma once

#include <cstdint>

#include "activities/Activity.h"

// Grid-based Minesweeper. Cursor navigation over the mine field with a small
// toolbar (cursorRow == -1) toggling between Reveal and Flag mode, since the
// device only exposes 6 logical buttons and has no dedicated flag button.
class MinesweeperActivity final : public Activity {
 private:
  static constexpr int CELL_SIZE = 48;  // tuned for ~12-14 columns on the physical panel
  static constexpr int MAX_CELLS = 256;  // covers the largest grid this screen can show at CELL_SIZE
  static constexpr float MINE_DENSITY = 0.15f;
  static constexpr int SUBHEADER_HEIGHT = 30;
  static constexpr int TOOLBAR_HEIGHT = 30;

  bool mine[MAX_CELLS]{};
  bool revealed[MAX_CELLS]{};
  bool flagged[MAX_CELLS]{};
  int8_t adjacentCount[MAX_CELLS]{};
  int floodStack[MAX_CELLS]{};  // manual stack for iterative flood-fill (member, not a large local)

  int cols = 0;
  int rows = 0;
  int gridOriginX = 0;
  int gridOriginY = 0;

  int cursorRow = -1;  // -1 = toolbar selected
  int cursorCol = 0;
  bool flagMode = false;
  bool firstReveal = true;
  bool gameOver = false;
  bool won = false;

  int mineCount = 0;
  int flaggedCount = 0;
  int revealedSafeCount = 0;

  void computeGrid();
  void resetGame();
  void placeMines(int excludeIndex);
  void revealCell(int row, int col);
  void toggleFlag(int row, int col);
  void checkWin();

 public:
  explicit MinesweeperActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Minesweeper", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
