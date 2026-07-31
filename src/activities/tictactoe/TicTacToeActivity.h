#pragma once

#include "activities/Activity.h"

class TicTacToeActivity final : public Activity {
 private:
  int board[3][3] = {0};  // 0 = empty, 1 = X, 2 = O

  int cursorRow = 1;  // cursorRow == -1 means the toolbar ("New Game") is selected
  int cursorCol = 1;

  bool xTurn = true;

  bool gameOver = false;
  bool isDraw = false;
  int winner = 0;  // 0 = none, 1 = X, 2 = O
  int winStartRow = -1, winStartCol = -1;
  int winEndRow = -1, winEndCol = -1;

  void resetGame();
  bool findWinner();  // sets winner + win{Start,End}{Row,Col}; returns true if a line is complete
  bool boardFull() const;

 public:
  explicit TicTacToeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TicTacToe", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
