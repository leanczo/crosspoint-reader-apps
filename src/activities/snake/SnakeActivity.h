#pragma once

#include <cstdint>

#include "activities/Activity.h"

enum class SnakeDirection : uint8_t { Up, Down, Left, Right };

struct SnakeCell {
  int8_t x;
  int8_t y;
};

// Grid-based Snake: the snake advances one cell per fixed tick (not per
// frame), matching the e-ink display's refresh capability instead of the
// classic arcade's continuous motion. Direction changes are queued and
// applied on the next tick, same pattern as a normal Snake's input buffer.
class SnakeActivity final : public Activity {
 private:
  static constexpr int CELL_SIZE = 20;
  static constexpr int MAX_CELLS = 1024;  // covers the largest grid this screen can show at CELL_SIZE
  static constexpr int SUBHEADER_HEIGHT = 30;
  static constexpr unsigned long TICK_MS = 900;

  SnakeCell body[MAX_CELLS];
  int snakeLength = 0;
  SnakeDirection direction = SnakeDirection::Right;
  SnakeDirection pendingDirection = SnakeDirection::Right;

  int cols = 0;
  int rows = 0;
  int gridOriginX = 0;
  int gridOriginY = 0;

  SnakeCell food{0, 0};
  int score = 0;
  bool gameOver = false;
  unsigned long lastTickMs = 0;

  void computeGrid();
  void resetGame();
  void placeFood();
  bool isOccupied(int x, int y, int checkLen) const;
  void tick();

 public:
  explicit SnakeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Snake", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
