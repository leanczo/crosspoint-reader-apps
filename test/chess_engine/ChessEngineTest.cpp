#include "ChessEngine.h"

#include <gtest/gtest.h>

namespace {

ChessEngine::ChessMove plainMove(int fr, int fc, int tr, int tc) {
  ChessEngine::ChessMove m;
  m.fromRow = static_cast<int8_t>(fr);
  m.fromCol = static_cast<int8_t>(fc);
  m.toRow = static_cast<int8_t>(tr);
  m.toCol = static_cast<int8_t>(tc);
  return m;
}

}  // namespace

TEST(ChessEngineTest, InitialPositionHas20LegalMovesForWhite) {
  ChessEngine::ChessState state;
  ChessEngine::setupInitialBoard(state);

  ChessEngine::ChessMove moves[ChessEngine::MAX_MOVES];
  int count = ChessEngine::generateLegalMoves(state, true, moves);
  EXPECT_EQ(count, 20);
}

TEST(ChessEngineTest, FoolsMateIsCheckmate) {
  ChessEngine::ChessState state;
  ChessEngine::setupInitialBoard(state);

  ChessEngine::applyMove(state, plainMove(6, 5, 5, 5));  // 1. f3
  ChessEngine::applyMove(state, plainMove(1, 4, 3, 4));  // 1... e5
  ChessEngine::applyMove(state, plainMove(6, 6, 4, 6));  // 2. g4
  ChessEngine::applyMove(state, plainMove(0, 3, 4, 7));  // 2... Qh4#

  EXPECT_TRUE(ChessEngine::isInCheck(state, /*white=*/true));
  EXPECT_EQ(ChessEngine::getGameStatus(state, /*sideToMoveIsWhite=*/true), ChessEngine::GameStatus::Checkmate);
}

TEST(ChessEngineTest, KingAndQueenPositionIsStalemateNotCheckmate) {
  ChessEngine::ChessState state;  // default: empty board
  state.board[7][7] = 6;          // White King h1
  state.board[6][5] = -6;         // Black King f2
  state.board[5][6] = -5;         // Black Queen g3

  EXPECT_FALSE(ChessEngine::isInCheck(state, true));
  EXPECT_EQ(ChessEngine::getGameStatus(state, true), ChessEngine::GameStatus::Stalemate);
}

TEST(ChessEngineTest, CastlingKingsideAvailableWhenClear) {
  ChessEngine::ChessState state;
  state.board[7][4] = 6;  // White King e1
  state.board[7][7] = 4;  // White Rook h1

  ChessEngine::ChessMove moves[ChessEngine::MAX_MOVES];
  int count = ChessEngine::generateLegalMoves(state, true, moves);

  bool foundCastle = false;
  for (int i = 0; i < count; i++) {
    if (moves[i].isCastleKingside) {
      foundCastle = true;
      EXPECT_EQ(moves[i].fromRow, 7);
      EXPECT_EQ(moves[i].fromCol, 4);
      EXPECT_EQ(moves[i].toRow, 7);
      EXPECT_EQ(moves[i].toCol, 6);
    }
  }
  EXPECT_TRUE(foundCastle);
}

TEST(ChessEngineTest, CastlingBlockedWhenKingInCheck) {
  ChessEngine::ChessState state;
  state.board[7][4] = 6;   // White King e1
  state.board[7][7] = 4;   // White Rook h1
  state.board[0][4] = -4;  // Black Rook e8 checks along the e-file

  ChessEngine::ChessMove moves[ChessEngine::MAX_MOVES];
  int count = ChessEngine::generateLegalMoves(state, true, moves);
  for (int i = 0; i < count; i++) {
    EXPECT_FALSE(moves[i].isCastleKingside);
  }
}

TEST(ChessEngineTest, CastlingBlockedWhenPassingThroughAttackedSquare) {
  ChessEngine::ChessState state;
  state.board[7][4] = 6;   // White King e1
  state.board[7][7] = 4;   // White Rook h1
  state.board[0][5] = -4;  // Black Rook f8 attacks f1, the king's pass-through square

  ChessEngine::ChessMove moves[ChessEngine::MAX_MOVES];
  int count = ChessEngine::generateLegalMoves(state, true, moves);
  for (int i = 0; i < count; i++) {
    EXPECT_FALSE(moves[i].isCastleKingside);
  }
}

TEST(ChessEngineTest, CastlingBlockedAfterKingHasMoved) {
  ChessEngine::ChessState state;
  state.board[7][4] = 6;
  state.board[7][7] = 4;
  state.whiteKingMoved = true;

  ChessEngine::ChessMove moves[ChessEngine::MAX_MOVES];
  int count = ChessEngine::generateLegalMoves(state, true, moves);
  for (int i = 0; i < count; i++) {
    EXPECT_FALSE(moves[i].isCastleKingside);
  }
}

TEST(ChessEngineTest, ApplyingCastleMoveRelocatesBothKingAndRook) {
  ChessEngine::ChessState state;
  state.board[7][4] = 6;
  state.board[7][7] = 4;

  ChessEngine::ChessMove castle = plainMove(7, 4, 7, 6);
  castle.isCastleKingside = true;
  ChessEngine::applyMove(state, castle);

  EXPECT_EQ(state.board[7][6], 6);  // King on g1
  EXPECT_EQ(state.board[7][5], 4);  // Rook on f1
  EXPECT_EQ(state.board[7][4], 0);
  EXPECT_EQ(state.board[7][7], 0);
}

TEST(ChessEngineTest, EnPassantCaptureAvailableImmediatelyAfterDoubleStep) {
  ChessEngine::ChessState state;
  state.board[3][4] = 1;   // White pawn e5
  state.board[1][3] = -1;  // Black pawn d7

  ChessEngine::applyMove(state, plainMove(1, 3, 3, 3));  // d7-d5

  EXPECT_EQ(state.enPassantRow, 2);
  EXPECT_EQ(state.enPassantCol, 3);

  ChessEngine::ChessMove moves[ChessEngine::MAX_MOVES];
  int count = ChessEngine::generateLegalMoves(state, true, moves);
  int enPassantIndex = -1;
  for (int i = 0; i < count; i++) {
    if (moves[i].isEnPassant) enPassantIndex = i;
  }
  ASSERT_NE(enPassantIndex, -1);
  EXPECT_EQ(moves[enPassantIndex].toRow, 2);
  EXPECT_EQ(moves[enPassantIndex].toCol, 3);

  ChessEngine::applyMove(state, moves[enPassantIndex]);
  EXPECT_EQ(state.board[3][3], 0);  // captured black pawn removed from d5
  EXPECT_EQ(state.board[2][3], 1);  // white pawn now on d6
}

TEST(ChessEngineTest, EnPassantExpiresAfterOneMove) {
  ChessEngine::ChessState state;
  state.board[3][4] = 1;   // White pawn e5
  state.board[1][3] = -1;  // Black pawn d7
  state.board[6][0] = 1;   // Unrelated white pawn for a waiting move

  ChessEngine::applyMove(state, plainMove(1, 3, 3, 3));  // d7-d5
  ChessEngine::applyMove(state, plainMove(6, 0, 5, 0));  // a2-a3 (waiting move)

  EXPECT_EQ(state.enPassantRow, -1);
  EXPECT_EQ(state.enPassantCol, -1);
}

TEST(ChessEngineTest, PawnAutoPromotesToQueenOnLastRank) {
  ChessEngine::ChessState state;
  state.board[1][0] = 1;  // White pawn one step from promoting, a7

  ChessEngine::ChessMove moves[ChessEngine::MAX_MOVES];
  int count = ChessEngine::generateLegalMoves(state, true, moves);
  ASSERT_EQ(count, 1);
  EXPECT_TRUE(moves[0].isPromotion);

  ChessEngine::applyMove(state, moves[0]);
  EXPECT_EQ(state.board[0][0], 5);  // promoted to queen
}

TEST(ChessEngineTest, BotCapturesHangingQueenInsteadOfIgnoringIt) {
  ChessEngine::ChessState state;
  state.board[7][4] = 6;   // White King e1
  state.board[0][7] = -6;  // Black King h8, far away and irrelevant here
  state.board[4][3] = 1;   // White pawn d4
  state.board[3][4] = -5;  // Undefended Black Queen on e5

  ChessEngine::ChessMove best;
  bool found = ChessEngine::findBestMove(state, /*white=*/true, /*depth=*/2, best);
  ASSERT_TRUE(found);
  EXPECT_EQ(best.fromRow, 4);
  EXPECT_EQ(best.fromCol, 3);
  EXPECT_EQ(best.toRow, 3);
  EXPECT_EQ(best.toCol, 4);
}
