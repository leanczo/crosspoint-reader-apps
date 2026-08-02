#include "ChessEngine.h"

#include <algorithm>
#include <cstdlib>

namespace ChessEngine {

namespace {

constexpr int PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6;

// Large but overflow-safe sentinels (not INT_MIN/MAX) since alpha-beta negates and
// compares these; using the full int range risks UB when negated.
constexpr int INF = 1'000'000;
constexpr int MATE_SCORE = 100'000;

bool inBounds(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }

bool isWhitePiece(int piece) { return piece > 0; }

bool isPathClear(const ChessState& state, int fromRow, int fromCol, int toRow, int toCol) {
  int dr = toRow - fromRow;
  int dc = toCol - fromCol;
  int stepR = (dr == 0) ? 0 : (dr > 0 ? 1 : -1);
  int stepC = (dc == 0) ? 0 : (dc > 0 ? 1 : -1);
  int r = fromRow + stepR;
  int c = fromCol + stepC;
  while (r != toRow || c != toCol) {
    if (state.board[r][c] != 0) return false;
    r += stepR;
    c += stepC;
  }
  return true;
}

// Whether the piece sitting on (pr, pc) attacks (tr, tc). Ignores whose turn it is,
// and unlike a legal move, a pawn "attacks" both of its diagonals even when empty
// (needed for check/castling-safety, not for generating pawn moves).
bool pieceAttacksSquare(const ChessState& state, int pr, int pc, int tr, int tc) {
  int piece = state.board[pr][pc];
  if (piece == 0) return false;
  bool white = isWhitePiece(piece);
  int absPiece = std::abs(piece);
  int dr = tr - pr;
  int dc = tc - pc;

  switch (absPiece) {
    case PAWN: {
      int dir = white ? -1 : 1;
      return dr == dir && std::abs(dc) == 1;
    }
    case KNIGHT:
      return std::abs(dr) * std::abs(dc) == 2;
    case BISHOP:
      return dr != 0 && std::abs(dr) == std::abs(dc) && isPathClear(state, pr, pc, tr, tc);
    case ROOK:
      return ((dr == 0) != (dc == 0)) && isPathClear(state, pr, pc, tr, tc);
    case QUEEN:
      return (dr != 0 || dc != 0) && (dr == 0 || dc == 0 || std::abs(dr) == std::abs(dc)) &&
             isPathClear(state, pr, pc, tr, tc);
    case KING:
      return std::max(std::abs(dr), std::abs(dc)) == 1;
    default:
      return false;
  }
}

void addMove(ChessMove out[MAX_MOVES], int& count, int fromRow, int fromCol, int toRow, int toCol,
             bool promotion = false, bool enPassant = false, bool castleKingside = false,
             bool castleQueenside = false) {
  if (count >= MAX_MOVES) return;  // defensive; real positions never approach this
  ChessMove& m = out[count++];
  m.fromRow = static_cast<int8_t>(fromRow);
  m.fromCol = static_cast<int8_t>(fromCol);
  m.toRow = static_cast<int8_t>(toRow);
  m.toCol = static_cast<int8_t>(toCol);
  m.isPromotion = promotion;
  m.isEnPassant = enPassant;
  m.isCastleKingside = castleKingside;
  m.isCastleQueenside = castleQueenside;
}

// Pseudo-legal: obeys per-piece movement, blocking, capture, castling and en-passant
// rules, but does not verify the mover's own king ends up safe afterward. That filter
// happens once, generically, in generateLegalMoves (it re-uses applyMove + isInCheck
// rather than duplicating "does this leave my king in check" per piece type).
int generatePseudoLegalMoves(const ChessState& state, bool white, ChessMove out[MAX_MOVES]) {
  int count = 0;
  const int dir = white ? -1 : 1;
  const int startRow = white ? 6 : 1;
  const int promoRow = white ? 0 : 7;
  const int homeRow = white ? 7 : 0;

  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      int piece = state.board[r][c];
      if (piece == 0 || isWhitePiece(piece) != white) continue;
      int absPiece = std::abs(piece);

      switch (absPiece) {
        case PAWN: {
          int r1 = r + dir;
          if (inBounds(r1, c) && state.board[r1][c] == 0) {
            addMove(out, count, r, c, r1, c, r1 == promoRow);
            int r2 = r + 2 * dir;
            if (r == startRow && state.board[r2][c] == 0) {
              addMove(out, count, r, c, r2, c);
            }
          }
          for (int dc = -1; dc <= 1; dc += 2) {
            int tc = c + dc;
            if (!inBounds(r1, tc)) continue;
            int target = state.board[r1][tc];
            if (target != 0 && isWhitePiece(target) != white) {
              addMove(out, count, r, c, r1, tc, r1 == promoRow);
            } else if (target == 0 && r1 == state.enPassantRow && tc == state.enPassantCol) {
              addMove(out, count, r, c, r1, tc, false, /*enPassant=*/true);
            }
          }
          break;
        }
        case KNIGHT: {
          static constexpr int offsets[8][2] = {{-2, -1}, {-2, 1}, {-1, -2}, {-1, 2},
                                                 {1, -2},  {1, 2},  {2, -1},  {2, 1}};
          for (const auto& off : offsets) {
            int tr = r + off[0], tc = c + off[1];
            if (!inBounds(tr, tc)) continue;
            int target = state.board[tr][tc];
            if (target == 0 || isWhitePiece(target) != white) addMove(out, count, r, c, tr, tc);
          }
          break;
        }
        case BISHOP:
        case ROOK:
        case QUEEN: {
          static constexpr int diagDirs[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
          static constexpr int straightDirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
          auto scan = [&](const int dirs[4][2]) {
            for (int d = 0; d < 4; d++) {
              int tr = r, tc = c;
              while (true) {
                tr += dirs[d][0];
                tc += dirs[d][1];
                if (!inBounds(tr, tc)) break;
                int target = state.board[tr][tc];
                if (target == 0) {
                  addMove(out, count, r, c, tr, tc);
                } else {
                  if (isWhitePiece(target) != white) addMove(out, count, r, c, tr, tc);
                  break;
                }
              }
            }
          };
          if (absPiece == BISHOP || absPiece == QUEEN) scan(diagDirs);
          if (absPiece == ROOK || absPiece == QUEEN) scan(straightDirs);
          break;
        }
        case KING: {
          for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
              if (dr == 0 && dc == 0) continue;
              int tr = r + dr, tc = c + dc;
              if (!inBounds(tr, tc)) continue;
              int target = state.board[tr][tc];
              if (target == 0 || isWhitePiece(target) != white) addMove(out, count, r, c, tr, tc);
            }
          }

          bool kingMoved = white ? state.whiteKingMoved : state.blackKingMoved;
          if (!kingMoved && r == homeRow && c == 4 && !isInCheck(state, white)) {
            bool rookKMoved = white ? state.whiteRookKingsideMoved : state.blackRookKingsideMoved;
            int rookKPiece = white ? ROOK : -ROOK;
            if (!rookKMoved && state.board[homeRow][7] == rookKPiece && state.board[homeRow][5] == 0 &&
                state.board[homeRow][6] == 0 && !isSquareAttacked(state, homeRow, 5, !white) &&
                !isSquareAttacked(state, homeRow, 6, !white)) {
              addMove(out, count, homeRow, 4, homeRow, 6, false, false, /*castleKingside=*/true);
            }

            bool rookQMoved = white ? state.whiteRookQueensideMoved : state.blackRookQueensideMoved;
            int rookQPiece = white ? ROOK : -ROOK;
            if (!rookQMoved && state.board[homeRow][0] == rookQPiece && state.board[homeRow][1] == 0 &&
                state.board[homeRow][2] == 0 && state.board[homeRow][3] == 0 &&
                !isSquareAttacked(state, homeRow, 2, !white) && !isSquareAttacked(state, homeRow, 3, !white)) {
              addMove(out, count, homeRow, 4, homeRow, 2, false, false, false, /*castleQueenside=*/true);
            }
          }
          break;
        }
        default:
          break;
      }
    }
  }
  return count;
}

void orderMovesCapturesFirst(const ChessState& state, ChessMove moves[], int count) {
  int insertPos = 0;
  for (int i = 0; i < count; i++) {
    bool isCapture = moves[i].isEnPassant || state.board[moves[i].toRow][moves[i].toCol] != 0;
    if (isCapture) {
      std::swap(moves[i], moves[insertPos]);
      insertPos++;
    }
  }
}

int minimax(ChessState state, bool sideIsWhite, int depth, int alpha, int beta) {
  ChessMove moves[MAX_MOVES];
  int count = generateLegalMoves(state, sideIsWhite, moves);
  if (count == 0) {
    if (!isInCheck(state, sideIsWhite)) return 0;  // stalemate
    return sideIsWhite ? -MATE_SCORE : MATE_SCORE;
  }
  if (depth == 0) return evaluateMaterial(state);

  orderMovesCapturesFirst(state, moves, count);

  if (sideIsWhite) {
    int best = -INF;
    for (int i = 0; i < count; i++) {
      ChessState next = state;
      applyMove(next, moves[i]);
      int score = minimax(next, false, depth - 1, alpha, beta);
      best = std::max(best, score);
      alpha = std::max(alpha, best);
      if (alpha >= beta) break;
    }
    return best;
  }
  int best = INF;
  for (int i = 0; i < count; i++) {
    ChessState next = state;
    applyMove(next, moves[i]);
    int score = minimax(next, true, depth - 1, alpha, beta);
    best = std::min(best, score);
    beta = std::min(beta, best);
    if (alpha >= beta) break;
  }
  return best;
}

}  // namespace

void setupInitialBoard(ChessState& state) {
  state = ChessState{};

  for (int c = 0; c < 8; c++) {
    state.board[1][c] = -PAWN;
    state.board[6][c] = PAWN;
  }
  state.board[0][0] = state.board[0][7] = -ROOK;
  state.board[7][0] = state.board[7][7] = ROOK;
  state.board[0][1] = state.board[0][6] = -KNIGHT;
  state.board[7][1] = state.board[7][6] = KNIGHT;
  state.board[0][2] = state.board[0][5] = -BISHOP;
  state.board[7][2] = state.board[7][5] = BISHOP;
  state.board[0][3] = -QUEEN;
  state.board[7][3] = QUEEN;
  state.board[0][4] = -KING;
  state.board[7][4] = KING;
}

bool isSquareAttacked(const ChessState& state, int row, int col, bool byWhite) {
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      int piece = state.board[r][c];
      if (piece == 0 || isWhitePiece(piece) != byWhite) continue;
      if (pieceAttacksSquare(state, r, c, row, col)) return true;
    }
  }
  return false;
}

bool isInCheck(const ChessState& state, bool white) {
  int kingPiece = white ? KING : -KING;
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (state.board[r][c] == kingPiece) return isSquareAttacked(state, r, c, !white);
    }
  }
  return false;  // no king on board; shouldn't happen in a real game state
}

int generateLegalMoves(const ChessState& state, bool white, ChessMove out[MAX_MOVES]) {
  ChessMove candidates[MAX_MOVES];
  int candidateCount = generatePseudoLegalMoves(state, white, candidates);

  int legalCount = 0;
  for (int i = 0; i < candidateCount; i++) {
    ChessState next = state;
    applyMove(next, candidates[i]);
    if (!isInCheck(next, white)) {
      out[legalCount++] = candidates[i];
    }
  }
  return legalCount;
}

void applyMove(ChessState& state, const ChessMove& move) {
  int piece = state.board[move.fromRow][move.fromCol];
  bool white = isWhitePiece(piece);

  // A captured rook loses castling rights on its side even though it never "moved" —
  // check before overwriting the destination square.
  int captured = state.board[move.toRow][move.toCol];
  if (captured == ROOK && move.toRow == 7 && move.toCol == 0) state.whiteRookQueensideMoved = true;
  if (captured == ROOK && move.toRow == 7 && move.toCol == 7) state.whiteRookKingsideMoved = true;
  if (captured == -ROOK && move.toRow == 0 && move.toCol == 0) state.blackRookQueensideMoved = true;
  if (captured == -ROOK && move.toRow == 0 && move.toCol == 7) state.blackRookKingsideMoved = true;

  if (move.isEnPassant) {
    state.board[move.fromRow][move.toCol] = 0;  // captured pawn sits beside the mover, not on the destination
  }

  state.board[move.toRow][move.toCol] = piece;
  state.board[move.fromRow][move.fromCol] = 0;

  if (move.isPromotion) {
    state.board[move.toRow][move.toCol] = white ? QUEEN : -QUEEN;
  }

  if (move.isCastleKingside) {
    state.board[move.fromRow][5] = state.board[move.fromRow][7];
    state.board[move.fromRow][7] = 0;
  } else if (move.isCastleQueenside) {
    state.board[move.fromRow][3] = state.board[move.fromRow][0];
    state.board[move.fromRow][0] = 0;
  }

  if (std::abs(piece) == KING) {
    if (white) state.whiteKingMoved = true;
    else state.blackKingMoved = true;
  }
  if (piece == ROOK && move.fromRow == 7 && move.fromCol == 0) state.whiteRookQueensideMoved = true;
  if (piece == ROOK && move.fromRow == 7 && move.fromCol == 7) state.whiteRookKingsideMoved = true;
  if (piece == -ROOK && move.fromRow == 0 && move.fromCol == 0) state.blackRookQueensideMoved = true;
  if (piece == -ROOK && move.fromRow == 0 && move.fromCol == 7) state.blackRookKingsideMoved = true;

  state.enPassantRow = -1;
  state.enPassantCol = -1;
  if (std::abs(piece) == PAWN && std::abs(move.toRow - move.fromRow) == 2) {
    state.enPassantRow = static_cast<int8_t>((move.fromRow + move.toRow) / 2);
    state.enPassantCol = move.fromCol;
  }
}

int evaluateMaterial(const ChessState& state) {
  static constexpr int VALUES[7] = {0, 100, 320, 330, 500, 900, 0};  // king excluded from material score
  int score = 0;
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      int piece = state.board[r][c];
      if (piece == 0) continue;
      int value = VALUES[std::abs(piece)];
      score += isWhitePiece(piece) ? value : -value;
    }
  }
  return score;
}

GameStatus getGameStatus(const ChessState& state, bool sideToMoveIsWhite) {
  ChessMove moves[MAX_MOVES];
  int count = generateLegalMoves(state, sideToMoveIsWhite, moves);
  bool check = isInCheck(state, sideToMoveIsWhite);
  if (count == 0) return check ? GameStatus::Checkmate : GameStatus::Stalemate;
  return check ? GameStatus::Check : GameStatus::InProgress;
}

bool findBestMove(const ChessState& state, bool white, int depth, ChessMove& bestMoveOut) {
  ChessMove moves[MAX_MOVES];
  int count = generateLegalMoves(state, white, moves);
  if (count == 0) return false;

  orderMovesCapturesFirst(state, moves, count);

  int bestIndex = 0;
  int bestScore = white ? -INF : INF;
  int alpha = -INF, beta = INF;
  for (int i = 0; i < count; i++) {
    ChessState next = state;
    applyMove(next, moves[i]);
    int score = minimax(next, !white, depth - 1, alpha, beta);
    if (white ? (score > bestScore) : (score < bestScore)) {
      bestScore = score;
      bestIndex = i;
    }
    if (white) alpha = std::max(alpha, bestScore);
    else beta = std::min(beta, bestScore);
  }
  bestMoveOut = moves[bestIndex];
  return true;
}

}  // namespace ChessEngine
