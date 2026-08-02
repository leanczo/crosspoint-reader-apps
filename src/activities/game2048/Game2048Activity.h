#pragma once

#include "activities/Activity.h"

enum class Game2048Direction { Left, Right, Up, Down };

class Game2048Activity final : public Activity {
 private:
  int board[4][4] = {{0}};
  int score = 0;
  bool gameOver = false;

  void resetGame();
  void spawnRandomTile();
  bool move(Game2048Direction direction);  // returns true if the board changed
  bool anyMovesLeft() const;

 public:
  explicit Game2048Activity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("2048", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
