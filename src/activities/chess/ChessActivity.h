#pragma once

#include <string>

#include "ChessEngine.h"
#include "activities/Activity.h"

enum class ChessMode { LocalTwoPlayer, VsBotEasy, VsBotMedium, VsBotHard };

class ChessActivity final : public Activity {
 private:
  ChessEngine::ChessState state;

  int cursorRow = 7;
  int cursorCol = 4;

  int selectedRow = -1;
  int selectedCol = -1;

  bool whiteTurn = true;
  bool flippedView = false;

  ChessMode mode = ChessMode::LocalTwoPlayer;
  ChessEngine::GameStatus gameStatus = ChessEngine::GameStatus::InProgress;

  // Bot search runs on a background FreeRTOS task (same pattern as
  // FootballActivity's fetch task) so loop() never blocks while the bot thinks.
  void* botTaskHandle = nullptr;
  volatile bool botMoveReady = false;
  bool botThinking = false;
  ChessEngine::ChessMove pendingBotMove;

  void resetGame();
  void refreshGameStatus();
  bool isBotTurn() const;
  int botSearchDepth() const;
  void startBotSearch();
  void cancelBotTask();
  const char* modeLabel() const;

 public:
  void runBotSearch();  // called from the FreeRTOS task trampoline

  explicit ChessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Chess", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
