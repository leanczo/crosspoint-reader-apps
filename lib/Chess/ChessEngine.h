#pragma once

#include <cstdint>

// Pure chess rules + a fixed-depth minimax bot. No Arduino/ESP-IDF/GfxRenderer
// dependencies so this compiles and is testable on the host (see test/chess_engine).
namespace ChessEngine {

// Board encoding: sign = color (positive white, negative black), magnitude = piece type.
// 1=Pawn, 2=Knight, 3=Bishop, 4=Rook, 5=Queen, 6=King, 0=Empty. Same convention ChessActivity
// used before this engine existed, kept so the UI layer needs no re-mapping.
struct ChessState {
  int board[8][8] = {};

  bool whiteKingMoved = false;
  bool blackKingMoved = false;
  bool whiteRookQueensideMoved = false;  // a1 rook
  bool whiteRookKingsideMoved = false;   // h1 rook
  bool blackRookQueensideMoved = false;  // a8 rook
  bool blackRookKingsideMoved = false;   // h8 rook

  // Square a pawn skipped over on its most recent double-step, capturable en passant
  // only on the immediately following move. -1 when there is no such square.
  int8_t enPassantRow = -1;
  int8_t enPassantCol = -1;
};

struct ChessMove {
  int8_t fromRow = -1;
  int8_t fromCol = -1;
  int8_t toRow = -1;
  int8_t toCol = -1;
  bool isCastleKingside = false;
  bool isCastleQueenside = false;
  bool isEnPassant = false;
  bool isPromotion = false;  // always promotes to queen, matching the pre-bot behavior
};

// Theoretical maximum number of legal moves in any reachable chess position.
// Used to size fixed (non-heap) move buffers.
static constexpr int MAX_MOVES = 218;

void setupInitialBoard(ChessState& state);

// True if any piece of color `byWhite` attacks (row, col), regardless of whether
// moving there would be legal (used for check and for castling's "king may not
// pass through an attacked square" rule).
bool isSquareAttacked(const ChessState& state, int row, int col, bool byWhite);

bool isInCheck(const ChessState& state, bool white);

// Fills `out` (capacity MAX_MOVES) with every legal move for `white`'s side —
// pseudo-legal moves for each piece, minus any that leave that side's own king
// in check. Returns the move count.
int generateLegalMoves(const ChessState& state, bool white, ChessMove out[MAX_MOVES]);

// Applies `move` to `state` in place, including castling's rook move, en passant's
// captured-pawn removal, promotion, and updating castling/en-passant bookkeeping.
// Does not check legality — only call with moves from generateLegalMoves.
void applyMove(ChessState& state, const ChessMove& move);

// Sum of standard material values (centipawns), positive favors white.
int evaluateMaterial(const ChessState& state);

enum class GameStatus { InProgress, Check, Checkmate, Stalemate };

// `sideToMoveIsWhite` must be the side whose turn it is in `state`.
GameStatus getGameStatus(const ChessState& state, bool sideToMoveIsWhite);

// Minimax with alpha-beta pruning, `depth` plies deep, material-only evaluation.
// Returns false if `white` has no legal moves (caller should not have asked).
bool findBestMove(const ChessState& state, bool white, int depth, ChessMove& bestMoveOut);

}  // namespace ChessEngine
