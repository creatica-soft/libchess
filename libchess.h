/// 
/// c++ -std=c++20 -Wall -shared -O3 -flto -DUSE_PTHREADS -DNDEBUG -DIS_64BIT -DUSE_POPCNT -DUSE_NEON=8 -DUSE_NEON_DOTPROD -Wl,-dylib,-rpath,$HOME/libchess -o libchess.dylib board.cpp engine.cpp fen.cpp pgn.cpp move.cpp tag.cpp zobrist-hash.cpp nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/nnue/features/full_threats.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp

/// Call Stockfish::Bitboards::init() once at start -- move generation segfaults without it.
/// (init_magic_bitboards()/cleanup_magic_bitboards() are gone; board.cpp uses attacks_bb<>.)

/// use -O0 -g for debugging with lldb or gdb instead of -O3 (lldb ./test, then run, and if crashes, bt)

/// The three lines below carry the same source list and the same flags as CMakeLists.txt.
/// They are not compile-tested -- this machine has no GCC and no Windows -- so treat them as
/// the shape of the build rather than as a verified command, and keep them in step with the
/// add_library(chess ...) line when a source is added or removed.
///
/// To compile on linux, run:
/// g++ -std=c++20 -Wall -shared -fPIC -O3 -march=native -DNDEBUG -o libchess.so board.cpp engine.cpp fen.cpp move.cpp pgn.cpp tag.cpp zobrist-hash.cpp nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/nnue/features/full_threats.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp
///

/// To build for Windows using mingw
/// g++ -std=c++20 -Wall -shared -O3 -march=native -DNDEBUG -o libchess.dll board.cpp engine.cpp fen.cpp move.cpp pgn.cpp tag.cpp zobrist-hash.cpp nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/nnue/features/full_threats.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp -Wl,--out-implib,libchess.dll.a
///
/// or in MSYS2 MINGW with clang
/// clang++ -std=c++20 -Wall -shared -O3 -march=native -flto -DNDEBUG -o libchess.dll board.cpp engine.cpp fen.cpp move.cpp pgn.cpp tag.cpp zobrist-hash.cpp nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/nnue/features/full_threats.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp -Wl,--out-implib,libchess.dll.a
///

/// HISTORICAL -- the cffi python bindings no longer regenerate, and the recipe below is kept
/// only as a record of how they were built before the C->C++20 rework. It was:
///
///   conda install cffi
///   cc -E libchess.h > libchess.ph
///   vi tasks.py
///   python3.12 tasks.py   (produced chess.cpython-312-darwin.so from libchess.so + libchess.ph)
///   python3.12 test.py    (exercised the chess module in chess.cpython-312-darwin.so)
///
/// Two things stop it. First, the scripts are not here: tasks.py, eval.py and the generated
/// libchess.ph all live in attic/ now, and test.py is gone altogether. Second, and the reason
/// it cannot simply be revived, cffi's cdef() parses a subset of C and this header stopped
/// being C a long time ago -- it declares 11 function templates, 52 CHESS_API functions that
/// take or return references, and 3 that return std::pair or std::tuple. None of those parse.
/// Reviving python bindings means writing a flat extern "C" shim over the API, not
/// preprocessing this file. The chess.cpython-312-darwin.so sitting in the repo root is a
/// leftover from June 2025 and does not match the current library.
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


//#include <stdbool.h>
#include <type_traits>
#include <utility>
#include <array>
#include <assert.h>
//#include <wchar.h>
//#include <stdio.h>

#ifndef LIBCHESS_H
#define LIBCHESS_H

#include "noise.h"
// enum Color, File, Square, PieceType and Piece, and struct Board, now have
// exactly ONE definition and it lives in chess_types.h. That header is shared
// with the vendored Stockfish fork (nnue/board.h forwards to it), which is why
// it is deliberately tiny: see the notes at the top of chess_types.h before
// moving anything else into it.
#include "chess_types.h"

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
#define MAX_FEN_STRING_LEN 128 //was 90; a maximal legal FEN plus NUL exceeds 90, and board2fen could overrun it
#define MAX_UCI_OPTION_NAME_LEN 32
#define MAX_UCI_OPTION_TYPE_LEN 8
#define MAX_UCI_OPTION_TYPE_NUM 5
//256, not 32. These hold FILE PATHS -- SyzygyPath, PolicyWeights, VisitDumpFile -- and 32
//bytes truncates any real one. It failed SILENTLY: setEngineStringOption() copies 31 bytes,
//the engine receives a path that does not exist, and nothing reports it. A truncated
//SyzygyPath in particular just falls back to slow online tablebase queries, which looks like
//the engine being mysteriously sluggish in endgames rather than like a configuration error.
//CHANGING THIS CHANGES THE LAYOUT OF struct Engine, so everything linking libchess must be
//rebuilt together -- `make -j` plus every compile line in the root. A binary built against the
//old layout and linked against the new library does not fail to load; it reads the wrong bytes.
//Raising 32 to 256 made test_pos report 678 failures until it was rebuilt, and 0 after.
#define MAX_UCI_OPTION_STRING_LEN 256
#define MAX_UCI_OPTION_BUTTON_NUM 4
//32, not 16. creatica advertises 17 spin options as of RepetitionGuard, and the seventeenth used
//to abort getOptions() outright -- see the note there. Headroom rather than exactly enough, because
//the failure mode of being one over is silent option loss in an unrelated category.
#define MAX_UCI_OPTION_SPIN_NUM 32
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

// NOT FILE_A..FILE_H: those names are enumerators of Stockfish::File in
// nnue/types.h, and as macros they silently miscompile nnue/bitboard.h's
// edge_distance() in any TU that sees libchess.h first.
#define FILE_A_BB 0x0101010101010101ULL
#define FILE_B_BB 0x0202020202020202ULL
#define FILE_C_BB 0x0404040404040404ULL
#define FILE_D_BB 0x0808080808080808ULL
#define FILE_E_BB 0x1010101010101010ULL
#define FILE_F_BB 0x2020202020202020ULL
#define FILE_G_BB 0x4040404040404040ULL
#define FILE_H_BB 0x8080808080808080ULL

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

inline constexpr uint64_t files_bb[] = { FILE_A_BB, FILE_B_BB, FILE_C_BB, FILE_D_BB, FILE_E_BB, FILE_F_BB, FILE_G_BB, FILE_H_BB };
inline constexpr uint64_t ranks_bb[] = { RANK1, RANK2, RANK3, RANK4, RANK5, RANK6, RANK7, RANK8 };
inline constexpr uint64_t en_passant_ranks[] = { RANK4, RANK5 };
inline constexpr uint64_t en_passant_files[] = {FILE_B_BB, FILE_A_BB | FILE_C_BB, FILE_B_BB | FILE_D_BB, FILE_C_BB | FILE_E_BB, FILE_D_BB | FILE_F_BB, FILE_E_BB | FILE_G_BB, FILE_F_BB | FILE_H_BB, FILE_G_BB };
inline constexpr uint64_t base_rank_bb[] = { RANK1, RANK8 };
inline constexpr uint64_t diag_bb[] = { DIAG_H1H1, DIAG_G1H2, DIAG_F1H3, DIAG_E1H4, DIAG_D1H5, DIAG_C1H6, DIAG_B1H7, DIAG_A1H8, 
                                    DIAG_A2G8, DIAG_A3F8, DIAG_A4E8, DIAG_A5D8, DIAG_A6C8, DIAG_A7B8, DIAG_A8A8 };
inline constexpr uint64_t antidiag_bb[] = { ADIAG_A1A1, ADIAG_A2B1, ADIAG_A3C1, ADIAG_A4D1, ADIAG_A5E1, ADIAG_A6F1, ADIAG_A7G1, 
                                        ADIAG_A8H1, ADIAG_B8H2, ADIAG_C8H3, ADIAG_D8H4, ADIAG_E8H5, ADIAG_F8H6, ADIAG_G8H7, ADIAG_H8H8 };

enum Castling : uint8_t { CastlingNone, CastlingKingside, CastlingQueenside, CastlingBoth };

inline constexpr const char * color[] = { "white", "black" };
inline constexpr char fenColor[] = { 'w', 'b' };

inline constexpr char enumFiles[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'N'};
enum Rank : uint8_t {Rank1, Rank2, Rank3, Rank4, Rank5, Rank6, Rank7, Rank8, RankNone, Rank_NB = 8};
inline constexpr char enumRanks[] = {'1', '2', '3', '4', '5', '6', '7', '8', 'N'};

inline constexpr const char * square[] = {
	"a1", "b1", "c1", "d1", "e1", "f1", "g1", "h1",
	"a2", "b2", "c2", "d2", "e2", "f2", "g2", "h2",
	"a3", "b3", "c3", "d3", "e3", "f3", "g3", "h3",
	"a4", "b4", "c4", "d4", "e4", "f4", "g4", "h4",
	"a5", "b5", "c5", "d5", "e5", "f5", "g5", "h5",
	"a6", "b6", "c6", "d6", "e6", "f6", "g6", "h6",
	"a7", "b7", "c7", "d7", "e7", "f7", "g7", "h7",
	"a8", "b8", "c8", "d8", "e8", "f8", "g8", "h8", "none"
};

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
inline constexpr const char * pieceType[] = {"any", "pawn", "knight", "bishop", "rook", "queen", "king", "none"};
//inline constexpr const char * pieceType[] = {"none", "pawn", "knight", "bishop", "rook", "queen", "king", "any"};
inline constexpr float pieceValue[] = { 0.0f, 0.1f, 0.30f, 0.32f, 0.50f, 0.90f, 1.0f }; //scaled down by kings value of 10
//ONE ENTRY PER PieceType VALUE, PieceTypeNone INCLUDED. The enum runs PieceTypeAny(0), Pawn(1)
//.. King(6), PieceTypeNone(7), so a seven-element table is indexed out of bounds by any code that
//looks up the value of "no piece" -- which get_see() does for every QUIET move, where the square
//it moves to is empty. That path only became reachable when SEE started being asked about quiet
//moves (Step 14 pruning); before that get_see() was only ever called on captures and the defect
//was latent. Both PieceTypeAny and PieceTypeNone are worth nothing, so both read 0.
inline constexpr int pieceValueCP[] = { 0, 100, 300, 320, 500, 900, 10000, 0 };
static_assert(sizeof pieceValueCP / sizeof pieceValueCP[0] == PieceTypeNone + 1,
              "pieceValueCP must cover every PieceType value, PieceTypeNone included");
inline constexpr float pieceMobility[] = { 0.0f, 4.0f, 8.0f, 11.0f, 14.0f, 25.0f, 8.0f }; //max value - used for norm
inline constexpr int MVV_LVA[7][7] = { //[attacker][victim]
    {0, 0, 0, 0, 0, 0, 0},       // None
    {0, 105, 205, 305, 405, 505, 605}, // Pawn attacking
    {0, 104, 204, 304, 404, 504, 604}, // Knight attacking
    {0, 103, 203, 303, 403, 503, 603}, // Bishop attacking
    {0, 102, 202, 302, 402, 502, 602}, // Rook attacking
    {0, 101, 201, 301, 401, 501, 601}, // Queen attacking
    {0, 100, 200, 300, 400, 500, 600}  // King attacking - last value is illegal, of course, king cannot attack king
};    

inline constexpr const char * piece[] = {
	"whites", "white pawn", "white knight", "white bishop", "white rook", "white queen", "white king", "none",
	"blacks", "black pawn", "black knight", "black bishop", "black rook", "black queen", "black king", "none"
};

inline constexpr char pieceLetter[] = {'C', 'P', 'N', 'B', 'R', 'Q', 'K', ' ', 'c', 'p', 'n', 'b', 'r', 'q', 'k', ' '};

// UCI promo letters, for SAN moves should be converted to uppercase
/*enum PromoLetter : uint8_t { PromoLetter_n = 2, PromoLetter_b, PromoLetter_r, PromoLetter_q};*/
inline constexpr char promoLetter[] = { '\0', '\0', 'N', 'B', 'R', 'Q', '\0', '\0' };
inline constexpr char uciPromoLetter[] = { '\0', '\0', 'n', 'b', 'r', 'q', '\0', '\0' };

//Move could be encoded as 15-bit int (promo << 12 | src << 6 | dst)
//or as 18-bit number (move_type << 15 | promo << 12 | src << 6 | dst)
//promo has 3 bits and uses 5 PieceType enum values {PieceTypeNone = 0, Knight = 2, Bishop, Rook or Queen}
//move_type has 3 bits
enum MoveType : uint8_t {MoveTypeEnPassantCapture, MoveTypeCastlingKingside, MoveTypeCastlingQueenside, MoveTypeNormal, MoveTypeCapture, MoveTypeEnPassant, MoveTypeNull};
inline constexpr const char * moveType[] = { "en passant capture", "castling kingside", "castling queenside", "normal", "capture", "en passant", "null" };

enum ProblemType : uint8_t { ProblemTypeNone, ProblemTypeBestMove, ProblemTypeAvoidMove };

enum GameStage : uint8_t { OpeningGame, MiddleGame, EndGame, FullGame };
inline constexpr const char * gameStage[] = { "opening", "middlegame", "endgame", "fullgame" };

inline constexpr const char * startPos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

inline constexpr Square castlingKingSquare[Color_NB][2] = { { SquareG1, SquareC1 }, { SquareG8, SquareC8 } };
inline constexpr Square castlingRookSquare[Color_NB][2] = { { SquareF1, SquareD1 }, { SquareF8, SquareD8 } };
inline constexpr Square initialRookSquare[Color_NB][2] = { { SquareH1, SquareA1 }, { SquareH8, SquareA8 } };
inline constexpr Piece castlingRook[Color_NB] = { WhiteRook, BlackRook };

inline constexpr Square SQ(Rank r, File f) {
    return static_cast<Square>((static_cast<int>(r) << 3) | static_cast<int>(f));
}
inline constexpr File SQ_FILE(Square sq) {
    return static_cast<File>(static_cast<int>(sq) & 7);
}
inline constexpr Rank SQ_RANK(Square sq) {
    return static_cast<Rank>(static_cast<int>(sq) >> 3);
}
inline constexpr uint64_t SQ_BIT(Square sq) {
    return 1ULL << static_cast<int>(sq);
}
inline constexpr uint64_t SQ_BIT(int sq) {
    return 1ULL << sq;
}
inline constexpr Diagonal SQ_DIAG(Square sq) {
    return static_cast<Diagonal>(7 + SQ_RANK(sq) - SQ_FILE(sq));
}
inline constexpr Antidiagonal SQ_ANTIDIAG(Square sq) {
    //Both operands are cast to int before the addition: adding two DIFFERENT enumeration types
    //is deprecated in C++20 and warned about in every translation unit that includes this header.
    return static_cast<Antidiagonal>(static_cast<int>(SQ_FILE(sq)) + static_cast<int>(SQ_RANK(sq)));
}
inline constexpr PieceType PC_TYPE(Piece pc) {
    return static_cast<PieceType>(static_cast<int>(pc) & 7);
}
inline constexpr Color PC_COLOR(Piece pc) {
    return static_cast<Color>(static_cast<int>(pc) >> 3);
}
inline constexpr Piece PC(Color c, PieceType pt) {
    return static_cast<Piece>(static_cast<int>(c) << 3 | static_cast<int>(pt));
}
inline constexpr Color OPP_COLOR(Color c) {
    return static_cast<Color>(static_cast<int>(c) ^ 1);
}
struct ChessPiece {
    Piece name = PieceNone;
    Square square = SquareNone;
};
inline constexpr void pc_init(ChessPiece * chess_piece, Piece pc, Square sq) {
    chess_piece->name = pc;
    chess_piece->square = sq;
}
inline constexpr Color SQ_COLOR(Square sq) {
    // (File ^ Rank) & 1 returns 0 for dark squares, 1 for light (or vice versa)
    // No 'if' statements means no branch mispredictions!
    return static_cast<Color>((static_cast<int>(SQ_FILE(sq)) ^ static_cast<int>(SQ_RANK(sq))) & 1);
}

// A helper to get the underlying integer value of any enum
template<typename T>
inline constexpr typename std::underlying_type<T>::type to_underlying(T e) {
    return static_cast<typename std::underlying_type<T>::type>(e);
}
// 1. Prefix Increment: ++sq, ++pt
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value>::type>
inline constexpr T& operator++(T& e) {
    return e = static_cast<T>(to_underlying(e) + 1);
}
// 1. Postfix Increment: sq++, pt++
// Returns the OLD value BY VALUE and leaves e incremented, as postfix must.
// The previous version did "T temp = e; ++e; return e = temp;" -- it incremented and then
// assigned the old value straight back, so sq++ left sq unchanged and returned a reference to
// it. Any "for (Square s = A; s < B; s++)" written against it was an infinite loop. Nothing in
// the built targets used it, which is the only reason this never surfaced.
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value>::type>
inline constexpr T operator++(T& e, int) {
    const T temp = e;
    e = static_cast<T>(to_underlying(e) + 1);
    return temp;
}
// 2. Prefix Decrement: --sq, --pt
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value>::type>
inline constexpr T& operator--(T& e) {
    return e = static_cast<T>(to_underlying(e) - 1);
}
// 3. Addition: sq + 8
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value>::type>
inline constexpr T operator+(T e, int i) {
    return static_cast<T>(to_underlying(e) + i);
}
// 4. Subtraction: sq - 8
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value>::type>
inline constexpr int operator-(T e, int i) {
    return static_cast<int>(to_underlying(e) - i);
}
// & is deliberately NOT overloaded for enums.
//
// There used to be an operator&(T, int) here whose body was "to_underlying(e) & 1" -- it threw
// its second operand away and always masked with one, so "piece & 7" silently evaluated to 0
// for every piece. It cost a real debugging session: a castling fix that tested
// "PC_TYPE(mover) == King" as "(mover & 7) == King" simply never fired, with no warning.
//
// Removing it is better than correcting it. Without an overload, "piece & 7" promotes to int
// and yields the right number, and anything that wants the result back as an enum has to say
// so with a cast -- which is where the intent belongs. For Piece specifically, use the named
// helpers PC_TYPE() and PC_COLOR() rather than masking by hand.
// 1. Addition Assignment: sq += 8
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value>::type>
inline constexpr T& operator+=(T& e, int i) {
    return e = static_cast<T>(to_underlying(e) + i);
}
// 2. Subtraction Assignment: sq -= 8
// Returns T&, like every other compound assignment. It was declared to return int while
// assigning an int to a T&, which is not a valid conversion for an unscoped enum -- so this
// template failed to compile the moment anything instantiated it. Nothing did.
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value>::type>
inline constexpr T& operator-=(T& e, int i) {
    return e = static_cast<T>(to_underlying(e) - i);
}
// Difference: Square - Square -> returns SIGNED int
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value>::type>
inline constexpr int operator-(T s1, T s2) {
    // We cast both to int BEFORE subtracting to prevent unsigned wrap-around
    return static_cast<int>(to_underlying(s1)) - static_cast<int>(to_underlying(s2));
}


inline constexpr uint8_t bitCount(unsigned long long value) {
#ifdef _MSC_VER
	return static_cast<uint8_t>(__popcnt64(value)); // equivalent to __builtin_popcountl
#else
  return static_cast<uint8_t>(__builtin_popcountll(value));
#endif
}

inline constexpr Square lsBit(uint64_t b) {
  assert(b);
	//if (b == 0) return SquareNone;
#ifdef _MSC_VER
	unsigned long index;
	_BitScanForward64(&index, b); // equivalent to __builtin_ctzl
	return static_cast<Square>(index);
#else
	return static_cast<Square>(__builtin_ctzll(b));
#endif
}

inline constexpr Square msBit(uint64_t b) {
  assert(b);
	//if (b == 0) return SquareNone;
#ifdef _MSC_VER
	unsigned long index;
	_BitScanReverse64(&index, b); // equivalent to __builtin_ctzl
	return static_cast<Square>(index);
#else
  return static_cast<Square>(63 - __builtin_clzll(b));
#endif
}

inline constexpr Square popLSB(uint64_t& b) {
  assert(b);
	//if (b == 0) return SquareNone;
	unsigned long index;
#ifdef _MSC_VER
	_BitScanForward64(&index, b); // equivalent to __builtin_ctzl
#else
	index = __builtin_ctzll(b);
#endif
  b &= b - 1; 
	return static_cast<Square>(index);
}

inline constexpr Square popMSB(uint64_t& b) {
  assert(b);
	//if (b == 0) return SquareNone;
	unsigned long index;
#ifdef _MSC_VER
	_BitScanReverse64(&index, b); // equivalent to __builtin_ctzl
#else
  index = (63 - __builtin_clzll(b));
#endif
  b ^= SQ_BIT(index); 
	return static_cast<Square>(index);
}

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

// Keep track of what a move changes on the board (used by NNUE).
// libchess.h is NOT self-contained: everything below needs Stockfish::Piece,
// Square, Color, Bitboard, DirtyPiece and DirtyThreats from nnue/types.h.
// Every translation unit must therefore include an nnue/ header (in practice
// "nnue/bitboard.h") BEFORE "libchess.h". That was already true - it was held
// up by a stale fallback copy of those types that never actually compiled.
#ifndef TYPES_H_INCLUDED
  #error "libchess.h requires nnue/types.h first -- put #include \"nnue/bitboard.h\" above #include \"libchess.h\"."
#endif
namespace Stockfish {

template<bool PutPiece>
inline void add_dirty_threat(
  DirtyThreats* const dts, Piece pc, Piece threatened, Square s, Square threatenedSq) {
    if (PutPiece) {
        dts->threatenedSqs |= SQ_BIT(threatenedSq);
        dts->threateningSqs |= SQ_BIT(s);
    }
    dts->list.push_back({pc, threatened, s, threatenedSq, PutPiece});
}

template<bool PutPiece, bool ComputeRay = true>
void update_piece_threats(Board& board, Piece pc, Square s, DirtyThreats * const dts, Bitboard noRaysContaining = -1ULL);

inline void put_piece(Board& board, Piece pc, Square s, DirtyThreats* const dts = nullptr) {
    board.piecesOnSquares[s] = static_cast<::Piece>(pc);
    uint64_t bitSq = SQ_BIT(s);
    board.side[PC_COLOR(static_cast<::Piece>(pc))] |= bitSq;
    board.pieceTypes[PC_TYPE(static_cast<::Piece>(pc)) - 1] |= bitSq;
    if (dts) update_piece_threats<true>(board, pc, s, dts);
}

inline void remove_piece(Board& board, const Square s, DirtyThreats* const dts = nullptr) {
    Piece pc = static_cast<Stockfish::Piece>(board.piecesOnSquares[s]);
    if (dts) update_piece_threats<false>(board, pc, s, dts);

    uint64_t bitSq = SQ_BIT(static_cast<Square>(s));
    board.side[PC_COLOR(static_cast<::Piece>(pc))] ^= bitSq;
    board.pieceTypes[PC_TYPE(static_cast<::Piece>(pc)) - 1] ^= bitSq;
    board.piecesOnSquares[s] = PieceNone;
}

inline void move_piece(Board& board, Square from, Square to, DirtyThreats* const dts) {
    Piece pc = static_cast<Stockfish::Piece>(board.piecesOnSquares[from]);
    uint64_t fromTo = SQ_BIT(from) | SQ_BIT(to);
    update_piece_threats<false>(board, pc, from, dts, fromTo);

    board.side[PC_COLOR(static_cast<::Piece>(pc))] ^= fromTo;
    board.pieceTypes[PC_TYPE(static_cast<::Piece>(pc)) - 1] ^= fromTo;
    board.piecesOnSquares[from] = PieceNone;
    board.piecesOnSquares[to] = static_cast<::Piece>(pc);
    update_piece_threats<true>(board, pc, to, dts, fromTo);
}

inline void swap_piece(Board& board, Square s, Piece pc, Stockfish::DirtyThreats* const dts) {
    Piece old = static_cast<Stockfish::Piece>(board.piecesOnSquares[s]);
    remove_piece(board, s);
    update_piece_threats<false, false>(board, old, s, dts);
    put_piece(board, pc, s);
    update_piece_threats<true, false>(board, pc, s, dts);
}

} //end of namespace Stockfish


constexpr std::array<std::pair<Rank, Rank>, 2> ep_ranks = [] { //pawnRank, enPassantRank;
  	std::array<std::pair<Rank, Rank>, 2> arr{};
  	arr[0] = std::make_pair(Rank5, Rank6);
  	arr[1] = std::make_pair(Rank4, Rank3);
  	return arr;
}();
  
//a line from sq1 to sq2 excludes sq1 and includes sq2 (same as Stockfish::BetweenBB[s1][s2])
/*constexpr std::array<std::array<uint64_t, 64>, 64> LineBetween = [] {
  auto my_abs = [](int x) { return x < 0 ? -x : x; };
  std::array<std::array<uint64_t, 64>, 64> arr{};
  for (Square s1 = SquareA1; s1 < SquareNone; ++s1) {
    for (Square s2 = SquareA1; s2 < SquareNone; ++s2) {
      arr[s1][s2] = 0;
      
      int r1 = SQ_RANK(s1), f1 = SQ_FILE(s1);
      int r2 = SQ_RANK(s2), f2 = SQ_FILE(s2);
      int dr = (r2 > r1) ? 1 : (r2 < r1) ? -1 : 0;
      int df = (f2 > f1) ? 1 : (f2 < f1) ? -1 : 0;

      // Only generate if they are on a line (Rank, File, or Diagonal)
      if (dr == 0 || df == 0 || my_abs(dr) == my_abs(df)) {
        Square curr = s1;
        while (curr != s2) {
					int new_r = SQ_RANK(curr) + dr;
	        int new_f = SQ_FILE(curr) + df;
	
	        // If we step off the 8x8 grid, stop immediately
	        if (new_r < 0 || new_r > 7 || new_f < 0 || new_f > 7) break;
	
	        curr = static_cast<Square>(new_r * 8 + new_f);
	        arr[s1][s2] |= SQ_BIT(curr);
	      }
      }
    }
  }
  return arr;
}();*/

struct CastlingData {
    uint64_t path;      // Must be vacant
    uint64_t checkZone; // Must not be attacked
};

//NO GLOBAL CASTLING TABLES. There used to be three -- CastlingPath[][], CastlingRights[64] and
//CastlingRooks[64] -- rebuilt by every fen2board() call from THAT position's king and rook squares, and
//read by castling generation and by every do_move, do_move_dp and ff_move in the process. So parsing
//any FEN changed the castling rules for every other board: an engine thread that parsed an endgame FEN
//while a search ran made the search castle through pieces and after its king had moved, and crashed it
//(35 crashes in 652 local match games). Everything they encoded follows from the board itself -- castling
//rights exist only while the king and that rook are still on the squares they started on -- so castling
//is now computed from the Board alone; see castling_path() and update_castling() in board.cpp.
extern uint64_t LineThrough[64][64];

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

/*struct KingSquare {
  File file = FileNone;
  Rank rank = RankNone;
  Diagonal diag = DiagonalNone;
  Antidiagonal antidiag = AntidiagonalNone;
  uint64_t bit = 0;
};*/

inline constexpr Rank baseRank[Color_NB] = { Rank1, Rank8 };
inline constexpr uint64_t occupations(const Board& board) { return board.side[ColorWhite] | board.side[ColorBlack];};
inline constexpr Square kingSquare(const Board& board, const Color color) { return lsBit(board.side[color] & board.pieceTypes[King - 1]);};

//history should be preserved in a separate stack
struct StateInfo {
    File enPassant = FileNone;
    uint8_t halfmoveClock = 0;
    MoveType type = MoveTypeNormal;
    PieceType capturedType = PieceTypeNone;
    //File castlingRook[Color_NB][2] = {{FileNone, FileNone}, {FileNone, FileNone}};
    uint8_t castlingRights;
    uint64_t castlingRooks;
    //isCheck, num_moves, isMate and isStaleMate were saved and restored here so that undo_move()
    //could put the Board's cached flags back. All four are GONE, on 2026-10-12, together with the
    //Board members themselves: isCheckMateStaleMate() returns a BoardState, so the Board carries no
    //derived state and there is nothing to preserve across a move.
    //
    //The note that lived here recorded the second of the three bugs this design caused: isMate and
    //isStaleMate were originally NOT saved, so undo_move() left the board wearing the flags of
    //whatever position was last examined, and any code calling isCheckMateStaleMate() on a child
    //mid-search corrupted its caller's view of the parent. It was fixed by APPENDING them rather
    //than inserting, because inserting would have shifted num_moves from offset 33 to 35 while
    //sizeof stayed 40 -- so a binary built against the older header would have read it from the
    //wrong byte when calling this dylib. That hazard is the reason to prefer returning a value
    //over growing a struct, and it is why these are removed rather than rearranged.
    // Pointers to previous state allow for repetition detection
    //StateInfo* previous; //currently not used
};

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

//DEPRECATED. These index optionSpin[]/optionString[]/optionCheck[] positionally, but
//getOptions() fills those arrays in the order the engine advertises its options, so the
//indices are only correct by coincidence. Use setEngineSpin()/setEngineCheck()/
//setEngineStringOption(), which look the option up by name. Kept because engine.cpp's
//getPV() and older unbuilt files still refer to them.
enum EngineSpinOptions : uint8_t {Hash, Threads, MultiPV, ExplorationMin, ExplorationMax, ExplorationDepthDecay, PVPlies, Temperature, VirtualLoss, ProbabilityMass, EvalScale, EvalDepth, MaxNodes, NegamaxDepth};
enum EngineStringOptions : uint8_t {SyzygyPath};
enum EngineCheckOptions : uint8_t {FinalInfoLines, IntermittentInfoLines, Ponder};

enum OptionType : uint8_t {
	Button, Check, Combo, Spin, String
};
constexpr const char * optionTypes[] = {
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
#else
	//The forked child was previously discarded, so the engine could be neither reaped
	//nor killed: every restart left a live 8-thread process behind. Plain int rather
	//than pid_t so this public header needs no extra include.
	int enginePid = -1;
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

constexpr const char * tags[] = {
	"Unknown", "Event", "Site", "Date", "Round", "White", "Black", "Result",
	"Annotator", "PlyCount", "TimeControl", "Time", "Termination", "Mode", "FEN", "SetUp", "Opening", "Variation", "Variant", "WhiteElo", "BlackElo", "ECO"
};

constexpr const char * ecotags[] = {
	"ECO", "Opening", "Variation"
};

enum Variant : uint8_t {
	Standard, Chess960
};

constexpr const char * variant[] = {
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
CHESS_API uint64_t getAttackedSquaresOnly(const Board& board);
//returns king_moves, pinned, pinning, checkers, kingSquare
CHESS_API std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, Square> kingMoves(Board& board);
//this is fast for determining multiple checkers instead of bitcount
CHESS_API std::pair<uint64_t, uint64_t> checkMask(const Board& board, Square kingSq, uint64_t checkers);
CHESS_API uint64_t getCheckers(const Board& board, const Square kingSq);
CHESS_API std::pair<uint64_t, uint64_t> pinFinder(const Board& board, const Square kingSq);
CHESS_API uint64_t piece_moves(Board& board, const PieceType pt, const Square sq, const Square kingSq, const uint64_t pinned, uint64_t pinning, const uint64_t check_mask, const uint64_t ep_mask);
//WHAT THE BOARD'S CACHED FLAGS USED TO CARRY, returned instead of stored.
//
//`Board::isCheck`/`isMate`/`isStaleMate`/`num_moves` are valid only IMMEDIATELY after kingMoves()
//or isCheckMateStaleMate() on that exact position -- do_move() preserves them so undo_move() can
//restore them, but never recomputes them, so after a move they describe the PARENT. Nothing in the
//type system said so, and the same bug has now been found three separate times: the flags leaking
//across positions in fen2board (see fen.cpp), isMate/isStaleMate not being saved in StateInfo (see
//the note there), and a stale isCheck making evaluate_nnue() return its in-check sentinel for a
//quiet position -- which cost a 4.3x slowdown and ten points of accuracy in an alpha-beta probe
//before anyone noticed.
//
//So the values are returned. The caller holds them for exactly as long as they are true.
struct BoardState {
    bool check     = false;   //the side to move is in check. SEE THE NOTE on mate, below.
    bool mate      = false;
    bool stalemate = false;
    int  num_moves = 0;       //legal moves for the side to move, promotions counted as four
};

//NOTE, PRESERVED AND NOT ENDORSED: when the position is mate this reports check == false, which is
//what the cached flags did (board.cpp set isMate and cleared isCheck together). A mate IS a check,
//so that is wrong on its face; it is kept byte-for-byte here because changing it would alter every
//caller's behaviour at the same time as the refactor that removed the fields, and the two need
//measuring apart.
CHESS_API BoardState isCheckMateStaleMate(Board& board);

//How many legal moves a generated bitboard represents for `pt`. A pawn landing on the promotion
//rank is FOUR moves, not one, and expanding them is the caller's job -- so a caller that counts
//with a bare bitCount() undercounts exactly where it matters. This is the rule piece_moves() used
//to apply to board.num_moves on the caller's behalf.
CHESS_API int moveCount(const Board& board, PieceType pt, uint64_t moves);

//=== THE NNUE WRAPPER API =========================================================================
//
//nnue/nnue.cpp is the thin wrapper the rest of the project uses, and until now it was declared
//NOWHERE PUBLIC: creatica_search.hpp, test_pos.cpp, test_nnue.cpp and test_smp.cpp each carried
//their own private copy of NNUEContext and these nine functions. Four copies that had to be kept
//in step by hand, and they had already drifted -- test_smp's NNUEContext default-initialised its
//pointers while the others left them indeterminate.
//
//That is not a tidiness problem. A declaration local to a translation unit makes the COMPILER
//agree with the caller and leaves the LINKER to discover the disagreement, or not: adding the
//in_check parameter to evaluate_nnue() produced exactly one error until all four copies were
//found, and a missed one would have linked against a mangled name that no longer existed, or
//matched a different overload. Declared once, a signature change breaks every caller at compile
//time, which is the only moment at which it is cheap.
//
//The Stockfish types appear only as pointers and references, so forward declarations are enough and
//libchess.h does not have to pull in the vendored accumulator headers. nnue/evaluate.h forward
//declares the same two in the same way; the struct/class tags match the definitions in
//nnue/types.h and nnue/nnue/nnue_accumulator.h, so a translation unit that includes both is fine.
namespace Stockfish {
struct DirtyPiece;
struct DirtyThreats;
namespace Eval::NNUE {
struct AccumulatorCaches;
class  AccumulatorStack;
}
}

//One per search thread. Owned by init_nnue_context()/free_nnue_context().
struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack = nullptr;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches = nullptr;
};

CHESS_API void init_nnue();          //loads the embedded nets; call once at program start
CHESS_API void cleanup_nnue();
CHESS_API void init_nnue_context(NNUEContext& ctx);
CHESS_API void free_nnue_context(NNUEContext& ctx);

//Returns PAWNS, from the side to move's perspective. `in_check` is passed in rather than read off
//the Board -- see the note on BoardState above for why that field could not be trusted. When it is
//true the net is not consulted and NNUE_CHECK is returned.
CHESS_API double evaluate_nnue(const Board& board, NNUEContext& ctx, bool in_check);
//THE SAME EVALUATION WITH STOCKFISH'S OPTIMISM TERM. nnue/evaluate.cpp already implements the full
//blend for Board -- the complexity scaling, the material term, all of it -- and evaluate_nnue()
//was simply passing 0. `optimism` is in STOCKFISH'S units, where a pawn is 208, because the
//constants it is blended against (7191, 77871) are. Stockfish derives it at the root as
//137 * avg / (|avg| + 91) for the side to move there, negated for the other side.
CHESS_API double evaluate_nnue_opt(const Board& board, NNUEContext& ctx, bool in_check, int optimism);

//Accumulator discipline: push -> do_move_dp -> ... -> undo_move -> pop, one context per thread.
//reset() is required before evaluating a position unrelated to the last one.
CHESS_API std::pair<Stockfish::DirtyPiece&, Stockfish::DirtyThreats&> accumulator_stack_push(NNUEContext& ctx);
CHESS_API void accumulator_stack_pop(NNUEContext& ctx);
CHESS_API void accumulator_stack_reset(NNUEContext& ctx);

//The human-readable evaluation trace, i.e. what the UCI "eval" command prints.
CHESS_API std::string nnue_eval(Board& board, bool in_check);

//The feature transformer's post-activation output -- the representation a policy head trains on.
//Maintained incrementally by the accumulator stack, so it is already computed at a search node.
CHESS_API int nnue_features(const Board& board, NNUEContext& ctx, unsigned char * out);
CHESS_API int nnue_feature_dims();
//==================================================================================================
//CHESS_API int get_see(const Board& board, const Move move, const PieceType capturedType); //move has been made
//STATIC EXCHANGE EVALUATION, for a move that has NOT been made yet: what the exchange on the
//destination square is worth once both sides have recaptured with their cheapest attacker each
//time, in centipawns from board.sideToMove's point of view. Negative means it loses material.
//Handles en passant and promotions. Revived 2026-10-12 -- see the note above the definition in
//board.cpp for the one case whose magnitude is unreliable.
CHESS_API int get_see(const Board& board, Move move);
//Every piece of either colour attacking `sq` under the given occupancy. Exported for get_see()'s
//x-ray walk, which removes each captured piece from `occupied` and re-asks.
CHESS_API uint64_t attackers_to(const Board& board, const Square sq, const uint64_t occupied);
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
//CHESS_API PieceType do_move_dp(Board& board, Move& move, StateInfo& state, Stockfish::DirtyPiece& dp);
CHESS_API PieceType do_move_dp(Board& board, Move& move, StateInfo& state, Stockfish::DirtyPiece& dp, Stockfish::DirtyThreats& dts);
CHESS_API void undo_move(Board& board, const Move& move, const StateInfo& state);

CHESS_API void zobristHash(Zobrist& z);
CHESS_API void getHash(ZobristHash& hash, const Board& board, const Zobrist& z);
CHESS_API void updateHash(ZobristHash& zh, const Board& board, const Move& move, const int capturedType, const Zobrist& z);

//returns en passant square (dst sq) if en passant capture from sq is legal or 0 otherwise
CHESS_API Square legalEnPassantMoveFromSq(const Board& board, const Square sq);

//returns en passant square (dst sq) if en passant capture is legal or 0 otherwise
CHESS_API Square legalEnPassantMove(const Board& board);


CHESS_API void stripGameResult(Game& game);
CHESS_API int normalizeMoves(char * moves);
CHESS_API int movesOnly(char * moves);

CHESS_API int strtotag(Tag tag, const char * tagString);
CHESS_API int strtoecotag(EcoTag, const char * tagString);
CHESS_API int gTags(Tag, FILE *);
CHESS_API int eTags(EcoTag, FILE *);

CHESS_API uint64_t countGames(FILE *, const char *, uint64_t gameStartPositions[], uint64_t maxNumberOfGames);


CHESS_API int initGame(Game& game, FILE *);
int playGame(Game& game);

CHESS_API void writeDebug(const Board& board);
CHESS_API void drawMoves(const Board& board, const Square sq, uint64_t moves);
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

//Set an option BY NAME on a spawned engine, and say so when the engine does not have it.
//
//These replace indexing engine.optionSpin[]/optionCheck[] with the fixed EngineSpinOptions /
//EngineCheckOptions enums below. Those arrays are filled by getOptions() in DISCOVERY ORDER --
//the order the engine happens to advertise its options in -- so the enum only addressed the
//right slot for engines that listed their options in exactly the enum's order. creatica does
//not: enum index 9 (ProbabilityMass) lands on its PolicyMode, index 10 (EvalScale) on its
//PolicyBlend, and on the check side index 0 (FinalInfoLines) lands on PerformanceCores. Writes
//past numberOfSpinOptions were quietly dropped instead, since setOptions() only walks that far.
//Either way the caller got no diagnostic.
//
//Spin values are clamped to the advertised min/max and the clamp is reported. Each returns
//false if the engine advertises no option of that name and type.
CHESS_API bool setEngineSpin(Engine& engine, const char * name, int64_t value);
CHESS_API bool setEngineCheck(Engine& engine, const char * name, bool value);
CHESS_API bool setEngineStringOption(Engine& engine, const char * name, const char * value);
//Advertised bounds, so a caller sweeping a parameter can stay inside them and can skip a
//parameter the engine does not have rather than tuning a value that goes nowhere.
CHESS_API bool engineSpinRange(const Engine& engine, const char * name,
                               int64_t& lo, int64_t& hi, int64_t& def);
CHESS_API bool isReady(const Engine& engine);
CHESS_API bool newGame(const Engine& engine);
CHESS_API void stop(const Engine& engine);
CHESS_API void quit(const Engine& engine);
CHESS_API bool position(const Engine& engine);
CHESS_API int go(const Engine& engine, Evaluation **);
CHESS_API float eval(const Engine& engine);
CHESS_API int getPV(const Engine& engine, Evaluation ** eval, const int multiPV);
CHESS_API int pieces(const Engine& engine); //non-standard UCI command pieces - returns the number of pieces on board

#endif

