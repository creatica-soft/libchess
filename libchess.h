/// 
/// c++ -std=c++20 -shared -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -Wl,-dylib,-rpath,/Users/ap/libchess -o libchess.dylib bitscanner.cpp board.cpp engine.cpp fen.cpp pgn.cpp move.cpp tag.cpp zobrist-hash.cpp magic_bitboards.cpp nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp

/// DON'T FORGET to init and free magic bitboards by calling init_magic_bitboards() and cleanup_magic_bitboards()

/// use -O0 -g for debugging with lldb or gdb instead of -O3 (lldb ./test, then run, and if crashes, bt)

/// To compile on alpine linux, run:
/// g++ -std=c++20 -shared -Wno-write-strings -Wno-deprecated -Wno-deprecated-declarations -Wno-strncat-size -fPIC -O3 -o libchess.so bitscanner.cpp board.cpp engine.cpp fen.cpp pgn.cpp move.cpp piece.cpp square.cpp tag.cpp zobrist-hash.cpp magic_bitboards.c nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp nnue/position.cpp
/// 


/// To build for Windows using mingw
/// g++ -std=c++20 -shared -Wno-write-strings -Wno-deprecated -Wno-deprecated-declarations -fPIC -O3 -o libchess.dll bitscanner.cpp board.cpp engine.cpp fen.cpp move.cpp piece.cpp square.cpp tag.cpp zobrist-hash.cpp magic_bitboards.c nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp nnue/position.cpp -Wl,--out-implib,libchess.dll.a
/// 
/// or in MSYS2 MINGW with clang
/// clang++ -std=c++20 -shared -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -o libchess.dll bitscanner.cpp board.cpp engine.cpp fen.cpp move.cpp piece.cpp square.cpp tag.cpp zobrist-hash.cpp magic_bitboards.c nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp nnue/position.cpp -Wl,--out-implib,libchess.dll.a
/// 

/// To build python bindings, use:
/// conda install cffi
/// cc -E libchess.h > libchess.ph
/// vi tasks.py
/// python3.12 tasks.py (it should produce chess.cpython-312-darwin.so from libchess.so and libchess.ph)
/// python3.12 test.py (to test chess module stored in chess.cpython-312-darwin.so)
///
#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
  #define CHESS_API __declspec(dllexport)
#else
  #define CHESS_API
#endif

#ifdef _WIN32
#define strtok_r strtok_s
#include "windows.h"
#endif


#include <stdbool.h>
//#include <wchar.h>
//#include <stdio.h>

#ifndef LIBCHESS_H
#define LIBCHESS_H

//#ifdef __cplusplus
//extern "C" {
//#endif

#include "noise.h"
//#include "noise2.h"

#define MAX_PIPE_NAME_LEN 256
#define MAX_ENGINE_NAME_LEN 128

#ifdef _WIN32
#define TO_ENGINE_NAMED_PIPE_PREFIX "\\\\.\\pipe\\to_engine_"
#define FROM_ENGINE_NAMED_PIPE_PREFIX "\\\\.\\pipe\\from_engine_"
#else
#define TO_ENGINE_NAMED_PIPE_PREFIX "/tmp/to_chess_engine_pipe"
#define FROM_ENGINE_NAMED_PIPE_PREFIX "/tmp/from_chess_engine_pipe"
#endif

#define MAX_NUMBER_OF_GAME_THREADS 16 //these threads just process pgn files
#define MAX_NUMBER_OF_SQL_THREADS 8 //these threads just update NextMovesX.db, where X is thread number. 
                                    //Use power of 2, i.e. 1, 2, 4, 8. 8 is max!
#define COMMIT_NEXT_MOVES_ROWS 5000000
#define COMMIT_GAMES_ROWS 10
#define MAX_SLEEP_COUNTER_FOR_SQLWRITER 16
#define MAX_NUMBER_OF_GAMES 256000
#define MAX_NUMBER_OF_ECO_LINES 2048
#define MAX_NUMBER_OF_GAME_MOVES 1024
#define MAX_NUMBER_OF_NEXT_MOVES 64
#define MAX_NUMBER_OF_TAGS 22
#define MAX_NUMBER_OF_ECO_TAGS 3
#define MAX_TAG_NAME_LEN 32
#define MAX_ECO_TAG_NAME_LEN 10
#define MAX_TAG_VALUE_LEN 90
#define MAX_SAN_MOVES_LEN 4096
#define MAX_UCI_MOVES_LEN 4096
#define MAX_ECO_MOVES_LEN 1024
#define MAX_FEN_STRING_LEN 90
#define MAX_UCI_OPTION_NAME_LEN 32
#define MAX_UCI_OPTION_TYPE_LEN 8
#define MAX_UCI_OPTION_TYPE_NUM 5
#define MAX_UCI_OPTION_STRING_LEN 32
#define MAX_UCI_OPTION_BUTTON_NUM 4
#define MAX_UCI_OPTION_SPIN_NUM 16
#define MAX_UCI_OPTION_CHECK_NUM 16
#define MAX_UCI_OPTION_COMBO_NUM 4
#define MAX_UCI_OPTION_COMBO_VARS 8
#define MAX_UCI_OPTION_STRING_NUM 8
#define MAX_UCI_MULTI_PV 8
#define MAX_VARIATION_PLIES 16
#define NO_MATE_SCORE 21000
#define MATE_SCORE 20000
#define INACCURACY 30
#define MISTAKE 75
#define BLUNDER 175

// Time management constants
#define MIN_MOVES_REMAINING 40
#define MAX_MOVES_REMAINING 80
#define TIME_SAFETY_BUFFER 5000 // 10s in ms
#define CRITICAL_TIME_FACTOR 1.5
#define MIN_TIME_THRESHOLD 10000 // 10s in ms
#define MIN_ITERATIONS 201
#define MAX_ITERATIONS 1000000001 // Safety cap

#define NNUE_CHECK 0.00001 //special value for check
#define STALE_MATE -0.00001 //special value for stalemate

#define FILE_A 0x0101010101010101ULL
#define FILE_B 0x0202020202020202ULL
#define FILE_C 0x0404040404040404ULL
#define FILE_D 0x0808080808080808ULL
#define FILE_E 0x1010101010101010ULL
#define FILE_F 0x2020202020202020ULL
#define FILE_G 0x4040404040404040ULL
#define FILE_H 0x8080808080808080ULL

#define RANK1 0x00000000000000FFULL
#define RANK2 0x000000000000FF00ULL
#define RANK3 0x0000000000FF0000ULL
#define RANK4 0x00000000FF000000ULL
#define RANK5 0x000000FF00000000ULL
#define RANK6 0x0000FF0000000000ULL
#define RANK7 0x00FF000000000000ULL
#define RANK8 0xFF00000000000000ULL
//diag = 7 - file + rank
#define DIAG_H1H1 0x0000000000000080ULL
#define DIAG_G1H2 0x0000000000008040ULL
#define DIAG_F1H3 0x0000000000804020ULL
#define DIAG_E1H4 0x0000000080402010ULL
#define DIAG_D1H5 0x0000008040201008ULL
#define DIAG_C1H6 0x0000804020100804ULL
#define DIAG_B1H7 0x0080402010080402ULL
#define DIAG_A1H8 0x8040201008040201ULL
#define DIAG_A2G8 0x4020100804020100ULL
#define DIAG_A3F8 0x2010080402010000ULL
#define DIAG_A4E8 0x1008040201000000ULL
#define DIAG_A5D8 0x0804020100000000ULL
#define DIAG_A6C8 0x0402010000000000ULL
#define DIAG_A7B8 0x0201000000000000ULL
#define DIAG_A8A8 0x0100000000000000ULL
//antidiag = file + rank
#define ADIAG_A1A1 0x0000000000000001ULL
#define ADIAG_A2B1 0x0000000000000102ULL
#define ADIAG_A3C1 0x0000000000010204ULL
#define ADIAG_A4D1 0x0000000001020408ULL
#define ADIAG_A5E1 0x0000000102040810ULL
#define ADIAG_A6F1 0x0000010204081020ULL
#define ADIAG_A7G1 0x0001020408102040ULL
#define ADIAG_A8H1 0x0102040810204080ULL
#define ADIAG_B8H2 0x0204081020408000ULL
#define ADIAG_C8H3 0x0408102040800000ULL
#define ADIAG_D8H4 0x0810204080000000ULL
#define ADIAG_E8H5 0x1020408000000000ULL
#define ADIAG_F8H6 0x2040800000000000ULL
#define ADIAG_G8H7 0x4080000000000000ULL
#define ADIAG_H8H8 0x8000000000000000ULL

static const uint64_t files_bb[] = { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H };
static const uint64_t ranks_bb[] = { RANK1, RANK2, RANK3, RANK4, RANK5, RANK6, RANK7, RANK8 };
static const uint64_t diag_bb[] = { DIAG_H1H1, DIAG_G1H2, DIAG_F1H3, DIAG_E1H4, DIAG_D1H5, DIAG_C1H6, DIAG_B1H7, DIAG_A1H8, 
                                    DIAG_A2G8, DIAG_A3F8, DIAG_A4E8, DIAG_A5D8, DIAG_A6C8, DIAG_A7B8, DIAG_A8A8 };
static const uint64_t antidiag_bb[] = { ADIAG_A1A1, ADIAG_A2B1, ADIAG_A3C1, ADIAG_A4D1, ADIAG_A5E1, ADIAG_A6F1, ADIAG_A7G1, 
                                        ADIAG_A8H1, ADIAG_B8H2, ADIAG_C8H3, ADIAG_D8H4, ADIAG_E8H5, ADIAG_F8H6, ADIAG_G8H7, ADIAG_H8H8 };

enum Castling : uint8_t { CastlingNone, CastlingKingside, CastlingQueenside, CastlingBoth };

enum Color : uint8_t { ColorWhite, ColorBlack, Color_NB };

static const char * color[] = { "white", "black" };
static const char fenColor[] = { 'w', 'b' };

enum File : uint8_t {FileA, FileB, FileC, FileD, FileE, FileF, FileG, FileH, FileNone, File_NB = 8};
static const char enumFiles[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'N'};
enum Rank : uint8_t {Rank1, Rank2, Rank3, Rank4, Rank5, Rank6, Rank7, Rank8, RankNone, Rank_NB = 8};
static const char enumRanks[] = {'1', '2', '3', '4', '5', '6', '7', '8', 'N'};

//redifinition of files_bb[] and ranks_bb[] above
//static uint64_t bitFiles[] = {FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H};
//static uint64_t bitRanks[] = {RANK1, RANK2, RANK3, RANK4, RANK5, RANK6, RANK7, RANK8};

// rank = square / 8, same as rank = square >> 3
// file = square % 8, same as file = square & 7
// square = rank * 8 + file, same as square = (rank << 3) | file
enum Square : uint8_t {
	SquareA1, SquareB1, SquareC1, SquareD1, SquareE1, SquareF1, SquareG1, SquareH1,
	SquareA2, SquareB2, SquareC2, SquareD2, SquareE2, SquareF2, SquareG2, SquareH2,
	SquareA3, SquareB3, SquareC3, SquareD3, SquareE3, SquareF3, SquareG3, SquareH3,
	SquareA4, SquareB4, SquareC4, SquareD4, SquareE4, SquareF4, SquareG4, SquareH4,
	SquareA5, SquareB5, SquareC5, SquareD5, SquareE5, SquareF5, SquareG5, SquareH5,
	SquareA6, SquareB6, SquareC6, SquareD6, SquareE6, SquareF6, SquareG6, SquareH6,
	SquareA7, SquareB7, SquareC7, SquareD7, SquareE7, SquareF7, SquareG7, SquareH7,
	SquareA8, SquareB8, SquareC8, SquareD8, SquareE8, SquareF8, SquareG8, SquareH8, SquareNone, Square_NB = 64, PawnSquare_NB = 48
};

static const char * square[] = {
	"a1", "b1", "c1", "d1", "e1", "f1", "g1", "h1",
	"a2", "b2", "c2", "d2", "e2", "f2", "g2", "h2",
	"a3", "b3", "c3", "d3", "e3", "f3", "g3", "h3",
	"a4", "b4", "c4", "d4", "e4", "f4", "g4", "h4",
	"a5", "b5", "c5", "d5", "e5", "f5", "g5", "h5",
	"a6", "b6", "c6", "d6", "e6", "f6", "g6", "h6",
	"a7", "b7", "c7", "d7", "e7", "f7", "g7", "h7",
	"a8", "b8", "c8", "d8", "e8", "f8", "g8", "h8", "none"
};

//int squareColor(int sqName); //use SQ_COLOR(sq) macro instead

enum Diagonal : uint8_t {
	DiagonalH1H1, DiagonalG1H2, DiagonalF1H3, DiagonalE1H4, DiagonalD1H5,
	DiagonalC1H6, DiagonalB1H7, DiagonalA1H8, DiagonalA2G8, DiagonalA3F8, 
	DiagonalA4E8, DiagonalA5D8, DiagonalA6C8, DiagonalA7B8, DiagonalA8A8, DiagonalNone, Diagonal_NB = 15
};

enum Antidiagonal : uint8_t {
	AntidiagonalA1A1, AntidiagonalA2B1, AntidiagonalA3C1, AntidiagonalA4D1, AntidiagonalA5E1,
	AntidiagonalA6F1, AntidiagonalA7G1, AntidiagonalA8H1, AntidiagonalB8H2, AntidiagonalC8H3,
	AntidiagonalD8H4, AntidiagonalE8H5, AntidiagonalF8H6, AntidiagonalG8H7, AntidiagonalH8H8, AntidiagonalNone, Antidiag_NB = 15
};
enum PieceType : uint8_t { PieceTypeAny, Pawn, Knight, Bishop, Rook, Queen, King, PieceTypeNone, PieceType_NB = 6, NonPawnType_NB = 5 };
static const char * pieceType[] = {"any", "pawn", "knight", "bishop", "rook", "queen", "king", "none"};

static const float pieceValue[] = { 0.0f, 0.1f, 0.30f, 0.32f, 0.50f, 0.90f, 1.0f }; //scaled down by kings value of 10
static const float pieceMobility[] = { 0.0f, 4.0f, 8.0f, 11.0f, 14.0f, 25.0f, 8.0f }; //max value - used for norm

// Piece enumeration: first three bits are used to encode the type, fourth bit defines the color, total 16 pieces
// Shifting Piece by 3 to the right gives PieceColor: color = piece >> 3
// Masking 3 lowest bits returns the PieceType: type = piece & 7
// PieceNone is exception to the above rules
// PieceNone has color white and type PieceTypeNone
enum Piece : uint8_t {
	PieceWhite, WhitePawn, WhiteKnight, WhiteBishop, WhiteRook, WhiteQueen, WhiteKing, PieceNone,
	PieceBlack, BlackPawn, BlackKnight, BlackBishop, BlackRook, BlackQueen, BlackKing, Piece_NB = 12, NonPawn_NB = 10
};

static const char * piece[] = {
	"whites", "white pawn", "white knight", "white bishop", "white rook", "white queen", "white king", "none",
	"blacks", "black pawn", "black knight", "black bishop", "black rook", "black queen", "black king"
};

/*enum PieceLetter : uint8_t { 
	PieceLetter_e, PieceLetter_P, PieceLetter_N, PieceLetter_B, PieceLetter_R, 
	PieceLetter_Q, PieceLetter_K, PieceLetter_X, PieceLetter_O, PieceLetter_p, PieceLetter_n,
	PieceLetter_b, PieceLetter_r, PieceLetter_q, PieceLetter_k, PieceLetter_x
};*/
static const char pieceLetter[] = {'C', 'P', 'N', 'B', 'R', 'Q', 'K', ' ', 'c', 'p', 'n', 'b', 'r', 'q', 'k', '*'};

// UCI promo letters, for SAN moves should be converted to uppercase
/*enum PromoLetter : uint8_t { PromoLetter_n = 2, PromoLetter_b, PromoLetter_r, PromoLetter_q};*/
static const char promoLetter[] = { '\0', '\0', 'N', 'B', 'R', 'Q', '\0', '\0' };
static const char uciPromoLetter[] = { '\0', '\0', 'n', 'b', 'r', 'q', '\0', '\0' };

//Move could be encoded as 15-bit int (promo << 12 | src << 6 | dst)
//or as 18-bit number (move_type << 15 | promo << 12 | src << 6 | dst)
//promo has 3 bits and uses 5 PieceType enum values {PieceTypeNone = 0, Knight = 2, Bishop, Rook or Queen}
//move_type has 3 bits
enum MoveType : uint8_t {MoveTypeNormal, MoveTypeCastlingKingside, MoveTypeCastlingQueenside, MoveTypeCapture, MoveTypeEnPassant, MoveTypeEnPassantCapture, MoveTypeNull};
static const char * moveType[] = { "normal", "castling kingside", "castling queenside", "capture", "en passant", "en passant capture", "null" };

enum ProblemType : uint8_t { ProblemTypeNone, ProblemTypeBestMove, ProblemTypeAvoidMove };

enum GameStage : uint8_t { OpeningGame, MiddleGame, EndGame, FullGame };
static const char * gameStage[] = { "opening", "middlegame", "endgame", "fullgame" };

static const char * startPos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Macros for on-fly computation (inline-able, zero cost)
#define SQ(rank, file) (Square(((rank) << 3) | (file)))
#define SQ_FILE(sq) (File((sq) & 7))
#define SQ_RANK(sq) (Rank((sq) >> 3))
//#define SQ_BIT(sq)  ((sq) == SquareNone ? 0 : (1ULL << (sq)))
#define SQ_BIT(sq)  (1ULL << (sq))
#define SQ_DIAG(sq) (Diagonal(7 + SQ_RANK(sq) - SQ_FILE(sq)))
#define SQ_ANTIDIAG(sq) (Antidiagonal(SQ_FILE(sq) + SQ_RANK(sq)))
#define SQ_COLOR(sq) (Color(((SQ_FILE(sq) ^ SQ_RANK(sq)) & 1) ? ColorWhite : ColorBlack))

#define PC_TYPE(pc) (PieceType((pc) & 7))
#define PC_COLOR(pc) (Color((pc) >> 3))
#define PC(color, type) (Piece(((color) << 3) | (type)))
#define PC_INIT(pc, pcName, sq) do { (pc)->name = (pcName); (pc)->square = (sq); } while (0)

#define OPP_COLOR(color) ((Color)((color) ^ 1))  // White=0, Black=1
#define PLY_NUM(board) (((board)->moveNumber - 1) * 2 + ((board)->sideToMove == ColorBlack)) //for white move 1, ply is 0

//having two hashes and verifying the second one when the first is the same for two positions,
//I've never seen a hash collision, so perhaps, one hash is enough
struct ZobristHash {
    uint64_t hash = 0;
    uint64_t prevCastlingRights = 0;
    uint64_t prevEnPassant = 0;
    //uint64_t prevHash = 0; //for debuging
    //24 bytes
};

//825 random 8-byte numbers from atmospheric noise
struct Zobrist {
    uint64_t blackMove = 0;
    uint64_t castling[16] = {0};
    uint64_t enPassant[File_NB] = {0};
    uint64_t emptySquares[Square_NB] = {0};
    uint64_t pawns[Color_NB][PawnSquare_NB]; //96 8-byte numbers
    uint64_t nonPawns[Color_NB][NonPawnType_NB][Square_NB] = {}; //640 8-byte numbers
    //6,600 bytes
};

struct ChessPiece {
    Piece name = PieceNone;
    Square square = SquareNone;
};

struct KingSquare {
  File file = FileNone;
  Rank rank = RankNone;
  Diagonal diag = DiagonalNone;
  Antidiagonal antidiag = AntidiagonalNone;
  uint64_t bit = 0;
};

//the smallest compact board representation could be as following:
//10 bits per piece at square (color << 9 | type << 6 | square) x 32 pieces = 320 bits or 40 bytes
//4 bits for castling, 4 bits for en passant (one byte)
//side to move 1 bit + half move clock 7 bits (one byte)
//move number one (for up to 256 moves) or two bytes for many more
//total 43 or 44 bytes
//is it practical to use it and unpack it to more convinient occupation bitboards every time?
//piecesOnSquares is redundant and can be derived from occupation bitboards in no more than 8 boolean ops
//movesFromSquares could be replaced with more compact array pieceMoves[32]
//the rest (moves, isCheck, isStaleMate, isMate) can be calculated from above
#ifndef BOARD_INCLUDED
struct Board {
    //these booleans are not strictly needed except maybe isChess960
    bool isCheck = false;
    bool isStaleMate = false;
    bool isMate = false;
    bool isChess960 = false;
    Color sideToMove = ColorWhite;
    File enPassant = FileNone;
    uint8_t halfmoveClock = 0;
    uint8_t num_moves = 0; //not necessary but good for 8-byte alignment on 64-bit systems
    //8 bytes up to here
    int moveNumber = 1; //may be unsigned short but will be padded 2 bytes anyway
    uint8_t castlingRook[Color_NB][2] = {{FileNone, FileNone}, {FileNone, FileNone}};
    //16 bytes up to here
    //the rest is aligned on 8-byte boundary
    Piece piecesOnSquares[Square_NB] = {PieceNone}; //64 bytes
    //uint64_t occupations[2][7] = {0}; //[color][pieceType] 14 8-byte bitboards
    //alternative compact representation has only 8 8-byte bitboards - much better!
    //for example, white knights = side[ColorWhite] & pieceTypes[Knight] - trade off between memory and cpu
    uint64_t side[Color_NB] = {0}; //all white and all black
    uint64_t pieceTypes[PieceType_NB] = {0}; //all pawns, knights, bishops, rooks, queens and kings
    //would be just 16 + 64 + 64 = 144 bytes instead of 192
    //192 bytes total
};
#endif
//history should be preserved in a separate stack
struct StateInfo {
    File enPassant = FileNone;
    uint8_t halfmoveClock = 0;
    MoveType type = MoveTypeNormal;
    PieceType capturedType = PieceTypeNone;
    File castlingRook[Color_NB][2] = {{FileNone, FileNone}, {FileNone, FileNone}};
    bool isCheck = false;
    // Pointers to previous state allow for repetition detection
    //StateInfo* previous; //currently not used
};

// Keep track of what a move changes on the board (used by NNUE)
#ifndef TYPES_H_INCLUDED
namespace Stockfish {
struct DirtyPiece {
    Piece pc = PieceNone;        // this is never allowed to be NO_PIECE
    Square from = SquareNone;
    Square to = SquareNone;  // to should be SQ_NONE for promotions

    // if {add,remove}_sq is SQ_NONE, {add,remove}_pc is allowed to be
    // uninitialized
    // castling uses add_sq and remove_sq to remove and add the rook
    Square remove_sq = SquareNone;
    Square add_sq = SquareNone;
    Piece remove_pc = PieceNone;
    Piece add_pc = PieceNone;
};
}
#endif


//Perhaps, we can wrap src, dst, promoType, type and capturedType and things for undo move in a 4-byte struct
struct Move {
  Square src = SquareNone;
  Square dst = SquareNone;
  PieceType promoType = PieceTypeNone;
  MoveType type = MoveTypeNormal;
};

struct MovesContext {
  int num_checkers = 0;
  Square checkerSquare = SquareNone; //used if num_checkers = 1
  uint64_t pinnedPieces = 0;
  uint64_t pinningPieces = 0;
  uint64_t blockingSquares = 0;
};

enum EngineSpinOptions : uint8_t {Hash, Threads, MultiPV, ExplorationMin, ExplorationMax, ExplorationDepthDecay, VirtualLoss, PVPlies, EvalScale, Temperature, ProbabilityMass, NegamaxDepth};
enum EngineStringOptions : uint8_t {SyzygyPath};
enum EngineCheckOptions : uint8_t {Ponder, FinalInfoLines, IntermittentInfoLines};

enum OptionType : uint8_t {
	Button, Check, Combo, Spin, String
};
static const char * optionTypes[] = {
	"button", "check", "combo", "spin", "string"
};

struct OptionSpin {
	char name[MAX_UCI_OPTION_NAME_LEN] = {};
	int64_t defaultValue = 0;
	int64_t value = 0;
	int64_t min = 0;
	int64_t max = 0;
};

struct OptionCheck {
	char name[MAX_UCI_OPTION_NAME_LEN] = {};
	bool defaultValue = false;
	bool value = false;
};

struct OptionString {
	char name[MAX_UCI_OPTION_NAME_LEN] = {};
	char defaultValue[MAX_UCI_OPTION_STRING_LEN] = {};
	char value[MAX_UCI_OPTION_STRING_LEN] = {};
};

struct OptionCombo {
	char name[MAX_UCI_OPTION_NAME_LEN] = {};
	char defaultValue[MAX_UCI_OPTION_STRING_LEN] = {};
	char values[MAX_UCI_OPTION_COMBO_VARS][MAX_UCI_OPTION_STRING_LEN] = {};
	char value[MAX_UCI_OPTION_STRING_LEN] = {};
};

struct OptionButton {
	char name[MAX_UCI_OPTION_NAME_LEN] = {};
	bool value = false; //if true, the button will be pressed
};

struct Engine {
	char id[MAX_UCI_OPTION_STRING_LEN] = {};
	char authors[2 * MAX_UCI_OPTION_STRING_LEN] = {};
	int numberOfCheckOptions = 0, numberOfComboOptions = 0, numberOfSpinOptions = 0, numberOfStringOptions = 0, numberOfButtonOptions = 0;
	OptionCheck optionCheck[MAX_UCI_OPTION_CHECK_NUM] = {};
	OptionCombo optionCombo[MAX_UCI_OPTION_COMBO_NUM] = {};
	OptionSpin optionSpin[MAX_UCI_OPTION_SPIN_NUM] = {};
	OptionString optionString[MAX_UCI_OPTION_STRING_NUM] = {};
	OptionButton optionButton[MAX_UCI_OPTION_BUTTON_NUM] = {};
	char engineName[255] = {};
	char namedPipeTo[255] = {};
	char namedPipeFrom[255] = {};
#ifdef _WIN32
	HANDLE hPipeToEngine = INVALID_HANDLE_VALUE;   // Add these to store pipe handles
	HANDLE hPipeFromEngine = INVALID_HANDLE_VALUE;
	HANDLE hProcess = INVALID_HANDLE_VALUE;
#endif
	char position[MAX_FEN_STRING_LEN] = {}; //FEN string
	char moves[MAX_UCI_MOVES_LEN] = {}; //UCI moves
	FILE * logfile = nullptr;
	//go() arguments
	int64_t movetime = 0;
	int depth = 0;
	uint64_t nodes = 0;
	int mate = 0;
	bool ponder = false;
	bool infinite = false;
	int64_t wtime = 0;
	int64_t btime = 0;
	int64_t winc = 0;
	int64_t binc = 0;
	int movestogo = 0;
	char * searchmoves = nullptr;
	FILE * toEngine = nullptr;
	FILE * fromEngine = nullptr;
};

struct Evaluation {
	int maxPlies = 0;
	int depth = 0;
	int seldepth = 0;
	int multipv = 0;
	int scorecp = 0;
	int matein = 0; //mate in <moves>, not <plies>
	uint64_t nodes = 0;
	uint64_t nps = 0;
	int hashful = 0; //permill (per thousand)
	int tbhits = 0;
	uint64_t time = 0; //ms
	char pv[1024] = {};
	char bestmove[6] = {};
	char ponder[6] = {};
	int nag = 0;
};

enum Tags : uint8_t {
	UnknownTag, Event, Site, Date, Round, White, Black, Result,
	Annotator, PlyCount, TimeControl, Time, Termination, Mode, FEN, SetUp, Opening, Variation, Variant, WhiteElo, BlackElo, ECO
};

enum EcoTags : uint8_t {
	eECO, eOpening, eVariation
};

static const char * tags[] = {
	"Unknown", "Event", "Site", "Date", "Round", "White", "Black", "Result",
	"Annotator", "PlyCount", "TimeControl", "Time", "Termination", "Mode", "FEN", "SetUp", "Opening", "Variation", "Variant", "WhiteElo", "BlackElo", "ECO"
};

static const char * ecotags[] = {
	"ECO", "Opening", "Variation"
};

enum Variant : uint8_t {
	Standard, Chess960
};

static const char * variant[] = {
	"Standard", "chess 960"
};

typedef char Tag[MAX_NUMBER_OF_TAGS][MAX_TAG_VALUE_LEN];

typedef char EcoTag[MAX_NUMBER_OF_ECO_TAGS][MAX_TAG_VALUE_LEN];

/// <summary>
/// Game class represents a PGN or EPD-formated chess game and may include tags (opcodes), moves and the game result
/// </summary>
struct Game {
	char sanMoves[MAX_SAN_MOVES_LEN] = {}; //space separated SAN moves without numbers 
	Tag tags = {};
	int numberOfPlies = 0;
};

struct EcoLine {
	char sanMoves[MAX_ECO_MOVES_LEN] = {}; // Space-separated moves in SAN format without numbers
	EcoTag tags = {}; // Chess eco header tag array
};

CHESS_API int randomNumber(const int, const int);

CHESS_API int fen2board(Board& board, const char * fenstr);
CHESS_API char * board2fen(const Board& board, char * fenString);
CHESS_API uint64_t getAttackedSquares(const Board& board, MovesContext& movesContext);
CHESS_API uint64_t getAttackedSquaresOnly(const Board& board);
CHESS_API Square getKingSquare(const Board& board, KingSquare& kingSq);
CHESS_API uint64_t kingMoves(Board& board, const Square kingSquare, const KingSquare& kingSq, MovesContext& movesContext, const uint64_t attackedSquares);
CHESS_API uint64_t piece_moves(const PieceType pieceType, const Square sq, const MovesContext& ctx, const KingSquare& kingSq, Board& board);
//CHESS_API uint64_t * generateMoves(struct Board * board, struct MovesContext * movesContext, const uint64_t attackedSquares, uint64_t * movesFromSquares);
CHESS_API void isCheckMateStaleMate(Board& board);
//CHESS_API void generateMoves(Board& board, uint64_t * movesFromSquares);
CHESS_API int uci2move_idx(const char * uci_move, Move& move);
CHESS_API char * idx2uci(const int move_idx, char * uci_move);
CHESS_API Move& idx2move(const int move_idx, Move& move);
CHESS_API int san2move(Board& board, const char * san_move, Move& move);
CHESS_API char * move2san(Board& board, const Move& move, char * sanMove);
CHESS_API bool promoMove(const Board& board, const Move& move);
//CHESS_API int initMove(Board * board, const char * moveString, uint64_t * movesFromSquares);
//fast-forward a valid uci move on a given board without init_move(), returns moveType enum
CHESS_API PieceType ff_move(Board& board, Move& move);
//CHESS_API void makeMove(Board * board, const int move);
CHESS_API PieceType do_move(Board& board, Move& move, StateInfo& state);
CHESS_API PieceType do_move_dp(Board& board, Move& move, StateInfo& state, Stockfish::DirtyPiece& dp);
CHESS_API void undo_move(Board& board, const Move& move, const StateInfo& state);

CHESS_API void zobristHash(Zobrist& z);
CHESS_API void getHash(ZobristHash& hash, const Board& board, const Zobrist& z);
CHESS_API void updateHash(ZobristHash& zh, const Board& board, const Move& move, const int capturedType, const Zobrist& z);

//returns en passant square (dst sq) if en passant capture from sq is legal or 0 otherwise
CHESS_API Square enPassantMoveLegal(Board& board, const Square sq);

//returns en passant square (dst sq) if en passant capture is legal or 0 otherwise
CHESS_API Square enPassantLegal(Board& board);

// two standard bit manupulation functions
CHESS_API uint8_t bitCount(uint64_t);
CHESS_API Square lsBit(uint64_t);

CHESS_API void stripGameResult(Game& game);
CHESS_API int normalizeMoves(char * moves);
CHESS_API int movesOnly(char * moves);

CHESS_API int strtotag(Tag tag, const char * tagString);
CHESS_API int strtoecotag(EcoTag, const char * tagString);
CHESS_API int gTags(Tag, FILE *);
CHESS_API int eTags(EcoTag, FILE *);

CHESS_API uint64_t countGames(FILE *, const char *, uint64_t gameStartPositions[], uint64_t maxNumberOfGames);

CHESS_API void cleanup_magic_bitboards(void);
CHESS_API void init_magic_bitboards(void);

CHESS_API int initGame(Game& game, FILE *);
int playGame(Game& game);

CHESS_API void writeDebug(const Board& board);
CHESS_API void drawMoves(const Board& board, const Square sq, const uint64_t * movesFromSquares);
CHESS_API int reconcile(const Board& board);
CHESS_API void getMoveType(char *, unsigned int);

CHESS_API void ecoClassify(Game& game, EcoLine **, int);

CHESS_API int engine(Engine& engine, const char * path);
CHESS_API void initChessEngine(Engine& chessEngine, const char * engineName, const int64_t movetime, const int depth, const int hashSize, const int threadNumber, const char * syzygyPath, const int multiPV, const bool logging, const bool limitStrength, const int elo);
CHESS_API void releaseChessEngine(Engine& chessEngine);
CHESS_API int nametoindex(const Engine& engine, const char * option, OptionType optionType);
CHESS_API int getOptions(Engine& engine);
CHESS_API int setOption(const Engine& engine, const char * option, OptionType optionType, void *);
CHESS_API void setOptions(const Engine& engine);
CHESS_API bool isReady(const Engine& engine);
CHESS_API bool newGame(const Engine& engine);
CHESS_API void stop(const Engine& engine);
CHESS_API void quit(const Engine& engine);
CHESS_API bool position(const Engine& engine);
CHESS_API int go(const Engine& engine, Evaluation **);
CHESS_API float eval(const Engine& engine);
CHESS_API int getPV(const Engine& engine, Evaluation ** eval, const int multiPV);
CHESS_API int pieces(const Engine& engine); //non-standard UCI command pieces - returns the number of pieces on board

//#ifdef __cplusplus
//}
//#endif
#endif

