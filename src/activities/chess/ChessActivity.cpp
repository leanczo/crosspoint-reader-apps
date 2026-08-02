#include "ChessActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Correct Midpoint Circle helper (Bresenham's)
void drawCircle(const GfxRenderer& renderer, int x0, int y0, int radius, int thickness, bool state) {
  for (int t = 0; t < thickness; t++) {
    int r = radius - t;
    int x = r;
    int y = 0;
    int err = 3 - 2 * r;
    while (x >= y) {
      renderer.drawPixel(x0 + x, y0 + y, state);
      renderer.drawPixel(x0 + y, y0 + x, state);
      renderer.drawPixel(x0 - y, y0 + x, state);
      renderer.drawPixel(x0 - x, y0 + y, state);
      renderer.drawPixel(x0 - x, y0 - y, state);
      renderer.drawPixel(x0 - y, y0 - x, state);
      renderer.drawPixel(x0 + y, y0 - x, state);
      renderer.drawPixel(x0 + x, y0 - y, state);

      if (err >= 0) {
        x -= 1;
        err += 4 * (y - x) + 10;
      } else {
        err += 4 * y + 6;
      }
      y += 1;
    }
  }
}

void fillCircle(const GfxRenderer& renderer, int x0, int y0, int radius, bool state) {
  int x = radius;
  int y = 0;
  int err = 3 - 2 * radius;
  while (x >= y) {
    renderer.drawLine(x0 - x, y0 + y, x0 + x, y0 + y, state);
    renderer.drawLine(x0 - y, y0 + x, x0 + y, y0 + x, state);
    renderer.drawLine(x0 - x, y0 - y, x0 + x, y0 - y, state);
    renderer.drawLine(x0 - y, y0 - x, x0 + y, y0 - x, state);
    if (err >= 0) {
      x -= 1;
      err += 4 * (y - x) + 10;
    } else {
      err += 4 * y + 6;
    }
    y += 1;
  }
}

bool sameSide(int piece, bool white) { return white ? piece > 0 : piece < 0; }

void chessBotTaskFunc(void* param) {
  auto* activity = static_cast<ChessActivity*>(param);
  activity->runBotSearch();
  vTaskDelete(nullptr);
}

}  // namespace

void ChessActivity::onEnter() {
  Activity::onEnter();
  bool isBoardEmpty = true;
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (state.board[r][c] != 0) {
        isBoardEmpty = false;
        break;
      }
    }
  }
  if (isBoardEmpty) {
    resetGame();
  }
  requestUpdate();
}

void ChessActivity::onExit() {
  // Unlike FootballActivity's network fetch task, the bot search task holds no
  // sockets/buffers that need graceful cleanup — it only touches local stack
  // memory — so killing it here on exit is safe.
  cancelBotTask();
  Activity::onExit();
}

void ChessActivity::resetGame() {
  ChessEngine::setupInitialBoard(state);
  cursorRow = 7;
  cursorCol = 4;
  selectedRow = -1;
  selectedCol = -1;
  whiteTurn = true;
  flippedView = false;
  cancelBotTask();
  botMoveReady = false;
  refreshGameStatus();
}

void ChessActivity::refreshGameStatus() { gameStatus = ChessEngine::getGameStatus(state, whiteTurn); }

bool ChessActivity::isBotTurn() const {
  return mode != ChessMode::LocalTwoPlayer && !whiteTurn && gameStatus != ChessEngine::GameStatus::Checkmate &&
         gameStatus != ChessEngine::GameStatus::Stalemate;
}

int ChessActivity::botSearchDepth() const {
  switch (mode) {
    case ChessMode::VsBotEasy:
      return 1;
    case ChessMode::VsBotMedium:
      return 2;
    case ChessMode::VsBotHard:
      return 3;
    default:
      return 1;
  }
}

const char* ChessActivity::modeLabel() const {
  switch (mode) {
    case ChessMode::LocalTwoPlayer:
      return tr(STR_CHESS_MODE_LOCAL);
    case ChessMode::VsBotEasy:
      return tr(STR_CHESS_MODE_BOT_EASY);
    case ChessMode::VsBotMedium:
      return tr(STR_CHESS_MODE_BOT_MEDIUM);
    case ChessMode::VsBotHard:
      return tr(STR_CHESS_MODE_BOT_HARD);
    default:
      return "";
  }
}

void ChessActivity::startBotSearch() {
  if (botTaskHandle != nullptr) return;
  botThinking = true;
  botMoveReady = false;
  // Stack accounts for ~4 nested minimax frames at depth 3 (findBestMove +
  // 3 recursive levels), each holding a ChessMove[MAX_MOVES] (~1.7KB) plus a
  // ChessState copy (~270B), roughly 8KB, doubled here for safety margin —
  // verify actual usage on-device with uxTaskGetStackHighWaterMark().
  xTaskCreate(chessBotTaskFunc, "chess_bot", 16384, this, 5, reinterpret_cast<TaskHandle_t*>(&botTaskHandle));
}

void ChessActivity::cancelBotTask() {
  if (botTaskHandle != nullptr) {
    TaskHandle_t tempHandle = static_cast<TaskHandle_t>(botTaskHandle);
    botTaskHandle = nullptr;
    vTaskDelete(tempHandle);
  }
  botThinking = false;
}

void ChessActivity::runBotSearch() {
  unsigned long startMs = millis();
  ChessEngine::ChessMove move;
  bool found = ChessEngine::findBestMove(state, /*white=*/false, botSearchDepth(), move);
  LOG_INF("CHESS", "Bot search done in %lums (depth=%d, found=%d)", millis() - startMs, botSearchDepth(),
          found ? 1 : 0);
  if (found) {
    pendingBotMove = move;
  }
  botMoveReady = true;
}

void ChessActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (selectedRow != -1) {
      selectedRow = -1;
      selectedCol = -1;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (isBotTurn()) {
    if (botTaskHandle == nullptr && !botMoveReady) {
      startBotSearch();
    }
    if (botMoveReady) {
      ChessEngine::applyMove(state, pendingBotMove);
      botTaskHandle = nullptr;  // task already self-deleted via vTaskDelete(nullptr)
      botMoveReady = false;
      botThinking = false;
      whiteTurn = true;
      refreshGameStatus();
      requestUpdate();
    }
    return;  // board/toolbar input ignored while the bot is thinking
  }

  const bool gameOver =
      gameStatus == ChessEngine::GameStatus::Checkmate || gameStatus == ChessEngine::GameStatus::Stalemate;

  if (cursorRow == -1) {
    // Toolbar navigation
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      cursorCol = (cursorCol - 1 + 3) % 3;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      cursorCol = (cursorCol + 1) % 3;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      cursorRow = flippedView ? 0 : 7;
      cursorCol = 4;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (cursorCol == 0) {
        resetGame();
        requestUpdate();
      } else if (cursorCol == 1) {
        flippedView = !flippedView;
        requestUpdate();
      } else {
        constexpr int kModeCount = 4;
        mode = static_cast<ChessMode>((static_cast<int>(mode) + 1) % kModeCount);
        resetGame();
        requestUpdate();
      }
    }
  } else {
    // Board navigation
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (flippedView) {
        if (cursorRow == 7) {
          cursorRow = -1;
          cursorCol = 0;
        } else {
          cursorRow++;
        }
      } else {
        if (cursorRow == 0) {
          cursorRow = -1;
          cursorCol = 0;
        } else {
          cursorRow--;
        }
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (flippedView) {
        if (cursorRow == 0) {
          cursorRow = -1;
          cursorCol = 0;
        } else {
          cursorRow--;
        }
      } else {
        if (cursorRow == 7) {
          cursorRow = -1;
          cursorCol = 0;
        } else {
          cursorRow++;
        }
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (flippedView) {
        cursorCol = (cursorCol + 1) % 8;
      } else {
        cursorCol = (cursorCol - 1 + 8) % 8;
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (flippedView) {
        cursorCol = (cursorCol - 1 + 8) % 8;
      } else {
        cursorCol = (cursorCol + 1) % 8;
      }
      requestUpdate();
    } else if (!gameOver && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      int clickedPiece = state.board[cursorRow][cursorCol];

      if (selectedRow == -1) {
        // First selection click
        if (clickedPiece != 0 && sameSide(clickedPiece, whiteTurn)) {
          selectedRow = cursorRow;
          selectedCol = cursorCol;
          requestUpdate();
        }
      } else {
        // Destination select click
        if (cursorRow == selectedRow && cursorCol == selectedCol) {
          // Deselect
          selectedRow = -1;
          selectedCol = -1;
          requestUpdate();
        } else if (clickedPiece != 0 && sameSide(clickedPiece, whiteTurn)) {
          // Select alternative own piece
          selectedRow = cursorRow;
          selectedCol = cursorCol;
          requestUpdate();
        } else {
          ChessEngine::ChessMove legalMoves[ChessEngine::MAX_MOVES];
          int legalCount = ChessEngine::generateLegalMoves(state, whiteTurn, legalMoves);
          for (int i = 0; i < legalCount; i++) {
            const auto& m = legalMoves[i];
            if (m.fromRow == selectedRow && m.fromCol == selectedCol && m.toRow == cursorRow &&
                m.toCol == cursorCol) {
              ChessEngine::applyMove(state, m);
              selectedRow = -1;
              selectedCol = -1;
              whiteTurn = !whiteTurn;
              refreshGameStatus();
              requestUpdate();
              break;
            }
          }
        }
      }
    }
  }
}

void ChessActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Chess");

  // Draw Toolbar: New Game / Flip Board / Mode, evenly spread across the screen
  // width so this still fits in portrait orientations (unlike the original
  // fixed 20/160px columns, which assumed a wide landscape screen).
  const int toolbarY = metrics.topPadding + metrics.headerHeight + 20;
  const int toolbarMargin = 20;
  const int toolbarGap = 16;
  const int toolbarBtnW = (pageWidth - 2 * toolbarMargin - 2 * toolbarGap) / 3;
  const int toolbarBtnX[3] = {toolbarMargin, toolbarMargin + toolbarBtnW + toolbarGap,
                               toolbarMargin + 2 * (toolbarBtnW + toolbarGap)};
  const char* toolbarLabels[3] = {tr(STR_CHESS_NEW_GAME), tr(STR_CHESS_FLIP_BOARD), modeLabel()};

  for (int i = 0; i < 3; i++) {
    bool sel = (cursorRow == -1 && cursorCol == i);
    renderer.drawRoundedRect(toolbarBtnX[i], toolbarY, toolbarBtnW, 30, 1, 5, true);
    if (sel) {
      renderer.fillRoundedRect(toolbarBtnX[i], toolbarY, toolbarBtnW, 30, 5, Color::Black);
    }
    int tW = renderer.getTextWidth(SMALL_FONT_ID, toolbarLabels[i]);
    int tx = toolbarBtnX[i] + std::max(4, (toolbarBtnW - tW) / 2);
    renderer.drawText(SMALL_FONT_ID, tx, toolbarY + 7, toolbarLabels[i], !sel);
  }

  // Chess Board dimensions
  const int cellS = 48;  // 48x48 squares to fit 48pt emoji font
  const int boardW = 8 * cellS;
  const int gridX = (pageWidth - boardW) / 2;
  const int gridY = toolbarY + 50;

  // Grid background outline
  renderer.drawRect(gridX - 2, gridY - 2, boardW + 4, boardW + 4, 2, true);

  // Legal destinations for the currently selected piece, computed once here
  // rather than per-square inside the loop below.
  ChessEngine::ChessMove selMoves[ChessEngine::MAX_MOVES];
  int selMoveCount = 0;
  if (selectedRow != -1) {
    selMoveCount = ChessEngine::generateLegalMoves(state, whiteTurn, selMoves);
  }

  // Draw Board Squares
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      int displayR = flippedView ? 7 - r : r;
      int displayC = flippedView ? 7 - c : c;

      int cx = gridX + displayC * cellS;
      int cy = gridY + displayR * cellS;

      // Checkerboard colors
      bool isDarkSquare = ((r + c) % 2 == 1);
      renderer.drawRect(cx, cy, cellS, cellS, 1, true);
      if (isDarkSquare) {
        // Draw diagonal pattern of black lines inside dark squares
        for (int i = 4; i < cellS; i += 8) {
          renderer.drawLine(cx + i, cy + 1, cx + 1, cy + i, 1, true);
          renderer.drawLine(cx + cellS - 2, cy + i, cx + i, cy + cellS - 2, 1, true);
        }
      }

      // Draw piece if present
      int piece = state.board[r][c];
      if (piece != 0) {
        const char* pieceStr = "";
        switch (piece) {
          case 1:
            pieceStr = "♙";
            break;  // White Pawn
          case 2:
            pieceStr = "♘";
            break;  // White Knight
          case 3:
            pieceStr = "♗";
            break;  // White Bishop
          case 4:
            pieceStr = "♖";
            break;  // White Rook
          case 5:
            pieceStr = "♕";
            break;  // White Queen
          case 6:
            pieceStr = "♔";
            break;  // White King
          case -1:
            pieceStr = "♟";
            break;  // Black Pawn
          case -2:
            pieceStr = "♞";
            break;  // Black Knight
          case -3:
            pieceStr = "♝";
            break;  // Black Bishop
          case -4:
            pieceStr = "♜";
            break;  // Black Rook
          case -5:
            pieceStr = "♛";
            break;  // Black Queen
          case -6:
            pieceStr = "♚";
            break;  // Black King
        }

        int tW = renderer.getTextWidth(NOTOSANS_16_EMOJI_FONT_ID, pieceStr);
        int tH = renderer.getTextHeight(NOTOSANS_16_EMOJI_FONT_ID);
        int px = cx + (cellS - tW) / 2;
        int py = cy + (cellS - tH) / 2;
        renderer.drawText(NOTOSANS_16_EMOJI_FONT_ID, px, py, pieceStr, true);
      }

      // Highlight selected piece
      if (selectedRow == r && selectedCol == c) {
        renderer.drawRect(cx + 2, cy + 2, cellS - 4, cellS - 4, 2, true);
      }

      // Draw dot if valid move destination for selected piece
      bool isLegalDestination = false;
      for (int i = 0; i < selMoveCount; i++) {
        if (selMoves[i].fromRow == selectedRow && selMoves[i].fromCol == selectedCol && selMoves[i].toRow == r &&
            selMoves[i].toCol == c) {
          isLegalDestination = true;
          break;
        }
      }
      if (isLegalDestination) {
        fillCircle(renderer, cx + cellS / 2, cy + cellS / 2, 4, true);
      }

      // Draw cursor cell outline
      if (cursorRow == r && cursorCol == c) {
        renderer.drawRect(cx, cy, cellS, cellS, 3, true);
      }
    }
  }

  // Footer status indicator
  const int footerY = gridY + boardW + 20;
  std::string status;
  if (botThinking) {
    status = tr(STR_CHESS_BOT_THINKING);
  } else if (gameStatus == ChessEngine::GameStatus::Checkmate) {
    status = whiteTurn ? tr(STR_CHESS_BLACK_WINS) : tr(STR_CHESS_WHITE_WINS);
  } else if (gameStatus == ChessEngine::GameStatus::Stalemate) {
    status = tr(STR_CHESS_STALEMATE);
  } else {
    status = whiteTurn ? tr(STR_CHESS_WHITE_TURN) : tr(STR_CHESS_BLACK_TURN);
    if (gameStatus == ChessEngine::GameStatus::Check) {
      status += " - ";
      status += tr(STR_CHESS_CHECK);
    } else if (selectedRow != -1) {
      status += " - ";
      status += tr(STR_CHESS_SELECT_DESTINATION);
    }
  }
  renderer.drawCenteredText(UI_12_FONT_ID, footerY, status.c_str(), true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
