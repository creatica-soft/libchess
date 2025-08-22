/// 
/// c++ -std=c++17 -shared -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -Xclang -fopenmp -Wl,-dylib,-lsqlite3,-lomp,-rpath,/opt/anaconda3/lib -I /opt/anaconda3/include -L/opt/anaconda3/lib -o libchess.so bitscanner.c board.c engine.c fen.c move.c piece.c square.c tag.c zobrist-hash.c sqlite.c my_md5.c magic_bitboards.c nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp nnue/position.cpp

/// DON'T FORGET to init and free magic bitboards by calling init_magic_bitboards() and cleanup_magic_bitboards()

/// use -O0 -g for debugging with lldb or gdb instead of -O3 (lldb ./test, then run, and if crashes, bt)

/// To compile on alpine linux, run:
/// g++ -std=c++17 -shared -Wno-write-strings -Wno-deprecated -Wno-deprecated-declarations -Wno-strncat-size -fPIC -O3 -o libchess.so bitscanner.c board.c engine.c fen.c move.c piece.c square.c tag.c zobrist-hash.c magic_bitboards.c nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp nnue/position.cpp
/// 


/// To build for Windows using mingw
/// g++ -std=c++17 -shared -Wno-write-strings -Wno-deprecated -Wno-deprecated-declarations -fPIC -O3 -o libchess.dll bitscanner.c board.c engine.cpp fen.c move.c piece.c square.c tag.c zobrist-hash.c magic_bitboards.c nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp nnue/position.cpp -Wl,--out-implib,libchess.dll.a
/// 
/// or in MSYS2 MINGW with clang
/// clang++ -std=c++17 -shared -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -o libchess.dll bitscanner.c board.c engine.c fen.c move.c piece.c square.c tag.c zobrist-hash.c magic_bitboards.c nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp nnue/position.cpp -Wl,--out-implib,libchess.dll.a
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
#endif


#include <stdbool.h>
//#include <wchar.h>
//#include <stdio.h>

#ifndef LIBCHESS_H
#define LIBCHESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "noise.h"
#include "noise2.h"

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
#define MAX_MOVES_REMAINING 60
#define TIME_SAFETY_BUFFER 5000 // 5s in ms
#define CRITICAL_TIME_FACTOR 1.5
#define MIN_TIME_THRESHOLD 10000 // 10s in ms
#define MIN_ITERATIONS 1001
#define MAX_ITERATIONS 1000000001 // Safety cap

#define NNUE_CHECK 0.00001 //special value for check

#define FILE_A 0x0101010101010101UL
#define FILE_B 0x0202020202020202UL
#define FILE_C 0x0404040404040404UL
#define FILE_D 0x0808080808080808UL
#define FILE_E 0x1010101010101010UL
#define FILE_F 0x2020202020202020UL
#define FILE_G 0x4040404040404040UL
#define FILE_H 0x8080808080808080UL

#define RANK1 0x00000000000000FFUL
#define RANK2 0x000000000000FF00UL
#define RANK3 0x0000000000FF0000UL
#define RANK4 0x00000000FF000000UL
#define RANK5 0x000000FF00000000UL
#define RANK6 0x0000FF0000000000UL
#define RANK7 0x00FF000000000000UL
#define RANK8 0xFF00000000000000UL

static unsigned long long files_bb[] = {FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H};
static unsigned long long ranks_bb[] = {RANK1, RANK2, RANK3, RANK4, RANK5, RANK6, RANK7, RANK8};

/*
// B-tree minimum degree (adjust based on performance needs)
#define T 32
typedef struct BTreeNode {
    unsigned long long keys[2 * T - 1];    // Array of keys
    struct BTreeNode *children[2 * T]; // Array of child pointers
    int num_keys;                 // Current number of keys
    bool is_leaf;                 // Leaf node flag
} BTreeNode;

bool BTreeSearch(BTreeNode *, unsigned long long);
BTreeNode* BTreeInsert(BTreeNode*, unsigned long long);
void BTreeCleanUp(BTreeNode * root, void * db);
*/

/// <summary>
/// Castling types enumeration
/// </summary>
enum CastlingSide { CastlingSideNone, CastlingSideKingside, CastlingSideQueenside, CastlingSideBoth };

/// <summary>
/// Enumeration of castling rights: lowest two bits for white, highest - for black
/// </summary>
enum CastlingRightsEnum {
	CastlingRightsWhiteNoneBlackNone = CastlingSideNone,
	CastlingRightsWhiteNoneBlackKingside = CastlingSideKingside << 2,
	CastlingRightsWhiteNoneBlackQueenside = CastlingSideQueenside << 2,
	CastlingRightsWhiteNoneBlackBoth = CastlingSideBoth << 2,
	CastlingRightsWhiteKingsideBlackNone = CastlingSideKingside,
	CastlingRightsWhiteQueensideBlackNone = CastlingSideQueenside,
	CastlingRightsWhiteBothBlackNone = CastlingSideBoth,
	CastlingRightsWhiteKingsideBlackKingside = CastlingSideKingside | (CastlingSideKingside << 2),
	CastlingRightsWhiteQueensideBlackKingside = CastlingSideQueenside | (CastlingSideKingside << 2),
	CastlingRightsWhiteBothBlackKingside = CastlingSideBoth | (CastlingSideKingside << 2),
	CastlingRightsWhiteKingsideBlackQueenside = CastlingSideKingside | (CastlingSideQueenside << 2),
	CastlingRightsWhiteQueensideBlackQueenside = CastlingSideQueenside | (CastlingSideQueenside << 2),
	CastlingRightsWhiteBothBlackQueenside = CastlingSideBoth | (CastlingSideQueenside << 2),
	CastlingRightsWhiteKingsideBlackBoth = CastlingSideKingside | (CastlingSideBoth << 2),
	CastlingRightsWhiteQueensideBlackBoth = CastlingSideQueenside | (CastlingSideBoth << 2),
	CastlingRightsWhiteBothBlackBoth = CastlingSideBoth | (CastlingSideBoth << 2)
};

/// <summary>
/// Color enumeration
/// </summary>
enum Color { ColorWhite, ColorBlack, ColorNone };

static const char * color[] = { "white", "black", "none" };

/// <summary>
/// File enumeration from a to h
/// </summary>
enum Files {FileA, FileB, FileC, FileD, FileE, FileF, FileG, FileH, FileNone};
static const char enumFiles[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'N'};
/// <summary>
/// Rank enumeration from 1 to 8
/// </summary>
enum Ranks {Rank1, Rank2, Rank3, Rank4, Rank5, Rank6, Rank7, Rank8, RankNone};
static const char enumRanks[] = {'1', '2', '3', '4', '5', '6', '7', '8', 'N'};

static unsigned long long bitFiles[] = {FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H};
static unsigned long long bitRanks[] = {RANK1, RANK2, RANK3, RANK4, RANK5, RANK6, RANK7, RANK8};

/// <summary>
/// Square enumeration
/// square / 8 = rank, square % 8 = file
/// </summary>
enum SquareName {
	SquareA1, SquareB1, SquareC1, SquareD1, SquareE1, SquareF1, SquareG1, SquareH1,
	SquareA2, SquareB2, SquareC2, SquareD2, SquareE2, SquareF2, SquareG2, SquareH2,
	SquareA3, SquareB3, SquareC3, SquareD3, SquareE3, SquareF3, SquareG3, SquareH3,
	SquareA4, SquareB4, SquareC4, SquareD4, SquareE4, SquareF4, SquareG4, SquareH4,
	SquareA5, SquareB5, SquareC5, SquareD5, SquareE5, SquareF5, SquareG5, SquareH5,
	SquareA6, SquareB6, SquareC6, SquareD6, SquareE6, SquareF6, SquareG6, SquareH6,
	SquareA7, SquareB7, SquareC7, SquareD7, SquareE7, SquareF7, SquareG7, SquareH7,
	SquareA8, SquareB8, SquareC8, SquareD8, SquareE8, SquareF8, SquareG8, SquareH8, SquareNone
};

static const char * squareName[] = {
	"a1", "b1", "c1", "d1", "e1", "f1", "g1", "h1",
	"a2", "b2", "c2", "d2", "e2", "f2", "g2", "h2",
	"a3", "b3", "c3", "d3", "e3", "f3", "g3", "h3",
	"a4", "b4", "c4", "d4", "e4", "f4", "g4", "h4",
	"a5", "b5", "c5", "d5", "e5", "f5", "g5", "h5",
	"a6", "b6", "c6", "d6", "e6", "f6", "g6", "h6",
	"a7", "b7", "c7", "d7", "e7", "f7", "g7", "h7",
	"a8", "b8", "c8", "d8", "e8", "f8", "g8", "h8", "none"
};

int squareColor(int sqName);

/// <summary>
/// Diagonal enumeration
/// </summary>
enum Diagonals {
	DiagonalH1H1, DiagonalG1H2, DiagonalF1H3, DiagonalE1H4, DiagonalD1H5, DiagonalC1H6, 
	DiagonalB1H7, DiagonalA1H8, DiagonalA2G8, DiagonalA3F8, DiagonalA4E8, DiagonalA5D8, 
	DiagonalA6C8, DiagonalA7B8, DiagonalA8A8, DiagonalNone
};

/// <summary>
/// Anti diagonals
/// </summary>
enum Antidiagonals {
	AntidiagonalA1A1, AntidiagonalA2B1, AntidiagonalA3C1, AntidiagonalA4D1, AntidiagonalA5E1,
	AntidiagonalA6F1, AntidiagonalA7G1, AntidiagonalA8H1, AntidiagonalB8H2, AntidiagonalC8H3,
	AntidiagonalD8H4, AntidiagonalE8H5, AntidiagonalF8H6, AntidiagonalG8H7, AntidiagonalH8H8, AntidiagonalNone
};

/// <summary>
/// Piece type enumeration
/// </summary>
enum PieceType {PieceTypeNone, Pawn, Knight, Bishop, Rook, Queen, King, PieceTypeAny};
static const char * pieceType[] = {"none", "pawn", "knight", "bishop", "rook", "queen", "king", "any"};

static float pieceValue[] = { 0.0f, 0.1f, 0.30f, 0.32f, 0.50f, 0.90f, 1.0f }; //scaled down by kings value of 10
static float pieceMobility[] = { 0.0f, 4.0f, 8.0f, 11.0f, 14.0f, 25.0f, 8.0f }; //max value - used for norm

/// <summary>
/// Piece enumeration: first three bits are used to encode the type, fourth bit defines the color.
/// Shifting PieceName by 3 to the right gives PieceColor: color = piece >> 3
/// Masking 3 lowest bits returns the PieceType: type = piece & 7
/// </summary>
enum PieceName {
	PieceNameNone,
	WhitePawn, WhiteKnight, WhiteBishop, WhiteRook, WhiteQueen, WhiteKing, PieceNameWhite, PieceNameAny,
	BlackPawn, BlackKnight, BlackBishop, BlackRook, BlackQueen, BlackKing, PieceNameBlack
};

static const char * pieceName[] = {
	"none", 
	"white pawn", "white knight", "white bishop", "white rook", "white queen", "white king", "whites", "any",
	"black pawn", "black knight", "black bishop", "black rook", "black queen", "black king", "blacks"
};

static const char pieceLetter[] = {' ', 'P', 'N', 'B', 'R', 'Q', 'K', 'C', '*', 'p', 'n', 'b', 'r', 'q', 'k', 'c'};

enum PieceLetter { 
	PieceLetter_e, PieceLetter_P, PieceLetter_N, PieceLetter_B, PieceLetter_R, 
	PieceLetter_Q, PieceLetter_K, PieceLetter_X, PieceLetter_O, PieceLetter_p, PieceLetter_n,
	PieceLetter_b, PieceLetter_r, PieceLetter_q, PieceLetter_k, PieceLetter_x
};

/// <summary>
/// UCI promo letters, for SAN moves should be converted to uppercase
/// </summary>
enum PromoLetter { PromoLetter_n = 2, PromoLetter_b, PromoLetter_r, PromoLetter_q};

static const char promoLetter[] = { '\0', '\0', 'N', 'B', 'R', 'Q' };
static const char uciPromoLetter[] = { '\0', '\0', 'n', 'b', 'r', 'q', '\0' };

enum MoveType {
	MoveTypeNormal, MoveTypeValid, MoveTypeCapture, MoveTypeCastlingKingside = 4, 
	MoveTypeCastlingQueenside = 8, MoveTypePromotion = 16, MoveTypeEnPassant = 32, 
	MoveTypeNull = 64
};

static const char * moveType[] = {
	"normal", "valid", "capture", "castling kingside", 
	"castling queenside", "promotion", "en passant", "null"
};

enum ProblemType { ProblemTypeNone, ProblemTypeBestMove, ProblemTypeAvoidMove };

enum GameStage { OpeningGame, MiddleGame, EndGame, FullGame };
static const char * gameStage[] = { "opening", "middlegame", "endgame", "fullgame" };

/// <summary>
/// Forthys-Edwards Notation for the position preceding a move 
/// </summary>
struct Fen {
	///<summary>
	/// FEN string as it is
	///</summary>
	char fenString[MAX_FEN_STRING_LEN];
	/// <summary>
	/// 1st field in FEN - ranks, separated by '/'
	/// </summary>
	char ranks[8][9];
	/// <summary>
	/// 2nd field in FEN - side to move
	/// </summary>
	int sideToMove;
	/// <summary>
	/// 3rd field in FEN - castling Rights
	/// </summary>
	int castlingRights;
	/// <summary>
	/// 4th field in FEN - EnPassant square - is set after double advancing a pawn regardless of whether an opposite side can capture or not.
	/// In X-FEN it is only setup when the opposite side can actually capture, although illegal captures (when pinned, for example) may not be checked
	/// </summary>
	int enPassant;
	unsigned long long enPassantLegalBit;
	/// <summary>
	/// 5th field in FEN - number of plies since last pawn advance or capture
	/// Used in 50-move draw rule
	/// </summary>
	int halfmoveClock;
	/// <summary>
	/// 6th field in FEN - full move number
	/// </summary>
	int moveNumber;
	/// <summary>
	/// True if castling rights are indicated by the castling rook file instead of letters for kingside or queenside
	/// </summary>
	bool isChess960;
	/// <summary>
	/// Castling rook files for chess 960 position indexed by side: 0 - kingside castling (short), 1 - queenside castling (long long) and by color: 0 - white, 1 - black
	/// </summary>
	int castlingRook[2][2];
	unsigned long long castlingBits; //rook squares if casling with that rook is allowed by FEN
};

enum EngineSpinOptions {Hash, Threads, MultiPV, ProbabilityMass, ExplorationConstant, DirichletAlpha, DirichletEpsilon};
enum EngineStringOptions {SyzygyPath};

///<summary>
/// UCI option types
///</summary>
enum OptionType {
	Button, Check, Combo, Spin, String
};

static const char * optionTypes[] = {
	"button", "check", "combo", "spin", "string"
};

///<summary>
/// UCI spin type option
///</summary>
struct OptionSpin {
	char name[MAX_UCI_OPTION_NAME_LEN];
	long long defaultValue;
	long long value;
	long long min;
	long long max;
};

///<summary>
/// UCI check type option
///</summary>
struct OptionCheck {
	char name[MAX_UCI_OPTION_NAME_LEN];
	bool defaultValue;
	bool value;
};

///<summary>
/// UCI string type option
///</summary>
struct OptionString {
	char name[MAX_UCI_OPTION_NAME_LEN];
	char defaultValue[MAX_UCI_OPTION_STRING_LEN];
	char value[MAX_UCI_OPTION_STRING_LEN];
};

///<summary>
/// UCI combo type option
///</summary>
struct OptionCombo {
	char name[MAX_UCI_OPTION_NAME_LEN];
	char defaultValue[MAX_UCI_OPTION_STRING_LEN];
	char values[MAX_UCI_OPTION_COMBO_VARS][MAX_UCI_OPTION_STRING_LEN];
	char value[MAX_UCI_OPTION_STRING_LEN];
};

///<summary>
/// UCI button type option
///</summary>
struct OptionButton {
	char name[MAX_UCI_OPTION_NAME_LEN];
	bool value; //if true, the button will be pressed
};

struct Engine {
	char id[MAX_UCI_OPTION_STRING_LEN];
	char authors[2 * MAX_UCI_OPTION_STRING_LEN];
	int numberOfCheckOptions, numberOfComboOptions, numberOfSpinOptions,
		numberOfStringOptions, numberOfButtonOptions;
	struct OptionCheck optionCheck[MAX_UCI_OPTION_CHECK_NUM];
	struct OptionCombo optionCombo[MAX_UCI_OPTION_COMBO_NUM];
	struct OptionSpin optionSpin[MAX_UCI_OPTION_SPIN_NUM];
	struct OptionString optionString[MAX_UCI_OPTION_STRING_NUM];
	struct OptionButton optionButton[MAX_UCI_OPTION_BUTTON_NUM];
	char engineName[255];
	char namedPipeTo[255];
	char namedPipeFrom[255];
	char position[MAX_FEN_STRING_LEN]; //FEN string
	char moves[MAX_UCI_MOVES_LEN]; //UCI moves
	FILE * logfile;
	//go() arguments
	long long movetime;
	int depth;
	int seldepth;
	int currentDepth;
	unsigned long long nodes;
	unsigned long long currentNodes;
	int mate;
	bool ponder;
	bool infinite;
	long long wtime;
	long long btime;
	long long winc;
	long long binc;
	int movestogo;
	char * searchmoves;
	FILE * toEngine;
	FILE * fromEngine;
};

struct Evaluation {
	unsigned char maxPlies;
	unsigned char depth;
	unsigned char seldepth;
	unsigned char multipv;
	int scorecp;
	int matein; //mate in <moves>, not <plies>
	unsigned long long nodes;
	unsigned long long nps;
	unsigned short hashful;//permill (per thousand)
	unsigned char tbhits;
	unsigned long long time; //ms
	char pv[1024];
	char bestmove[6];
	char ponder[6];
	unsigned char nag;
};

/// <summary>
///  Converts FEN string to struct fen
/// </summary>
CHESS_API int strtofen(struct Fen *, const char *);

/// <summary>
///  Updates fenString in Fen struct
/// </summary>
int fentostr(struct Fen *);

/// <summary>
/// Square class represents chess board square
/// </summary>
struct Square {
	int name;
	unsigned long long bitSquare;
	int file;
	int rank;
	int diag;
	int antiDiag;
};

/// <summary>
/// ChessPiece struct
/// </summary>
struct ChessPiece {
	int name;
	int type;
	int color;
	struct Square square;
};

/// <summary>
/// Board struct represents chess board
/// </summary>
struct Board {
	//unsigned long long defendedPieces; //defended opponent pieces 
	//unsigned long long attackedPieces; //sideToMove pieces attacked by opponent
	//unsigned long long attackedSquares; //all squares attacked by opponent
	//unsigned long long blockingSquares;
	//unsigned long long checkers;
	//unsigned long long pinnedPieces;
	//unsigned long long pinningPieces;
	//unsigned long long oPawnMoves, oKnightMoves, oBishopMoves, oRookMoves, oQueenMoves, oKingMoves, pawnMoves, knightMoves, bishopMoves, rookMoves, queenMoves, kingMoves;
	unsigned long long occupations[16];
	int piecesOnSquares[64]; //array of enum PieceName
	unsigned long long moves; //all sideToMoveMoves
	unsigned long long movesFromSquares[64]; //these include opponents moves as well
	unsigned long long sideToMoveMoves[64]; //these are just the moves of sideToMove
	//unsigned long long channel[64]; //AI model input 21 channels (16 for each piece + 5 for promotions)
	//unsigned long long sourceSquare[10]; //source square for AI channel[64] 
	int opponentColor; //enum Color
	int plyNumber;
	int capturedPiece; //enum PieceName
	int promoPiece; //enum PieceName
	struct ChessPiece movingPiece;
	bool isCheck;
	bool isStaleMate;
	bool isMate;
	struct Fen * fen;
	struct ZobristHash * zh;
};

struct Move {
	int type;
	struct Square sourceSquare;
	struct Square destinationSquare;
	char sanMove[12];
	char uciMove[6];
	struct Board * chessBoard;
	int castlingRook;
	//int prevCastlingRights;
	//int prevEnPassant;
	//int prevHalfmoveClock;
	//int prevCastlingRook[2][2];
	//int capturedPiece;
};

struct BMPR {
  long long samples;
  long long sample;
  long long channels;
  float * boards_legal_moves; // [batch_size, number_of_channels, 8, 8]
  //long long * move_type; // piece channel [0, 10] [batch_size]
  //long long * src_sq;
  //long long * dst_sq;
  long long * move; // encoded as src_sq * 64 + dst_sq
  long long * result; // [batch_size]
  //int * stage; // [batch_size]
};

static const char * startPos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
static const unsigned long long STARTPOS_HASH = 0x958ee7dbd87b2d0aUL;
static const unsigned long long STARTPOS_HASH2 = 0x4ed9a976a0f95be5UL;
static const unsigned long long STARTPOS_CASTLING_RIGHTS = 0x85a2e5d9b69ed995UL;
static const unsigned long long STARTPOS_CASTLING_RIGHTS2 = 0x8057e61321fd55e8UL;

struct ZobristHash {
	/// <summary>
	/// Zobrist hash of a given board 
	/// </summary>
	unsigned long long hash;
	unsigned long long hash2;
	unsigned long long blackMove;
	unsigned long long blackMove2;
	unsigned long long prevCastlingRights;
	unsigned long long prevCastlingRights2;
	unsigned long long prevEnPassant;
	unsigned long long prevEnPassant2;
	unsigned long long castling[16];
	unsigned long long castling2[16];
	unsigned long long enPassant[8];
	unsigned long long enPassant2[8];
	unsigned long long piecesAtSquares[13][64];
	unsigned long long piecesAtSquares2[13][64];
	/// <summary>
	/// Hash of the standard chess starting position
	/// </summary>
//	unsigned long long startPosHash;
};

struct MoveScoreGames {
  char move[6]; //uci move
  int score; //position score, i.e. sum of wins and losses by making this move
  unsigned int games;
  int scorecp; // evaluated by a chess engine but negative values meaning black winning, positive - white
};

struct MoveScores {
  char move[6]; //uci move
  double score; //weighted score, i.e. score / total number of games in NextMoves.db for a given position
  int scorecp;
};

enum Tags {
	UnknownTag, Event, Site, Date, Round, White, Black, Result,
	Annotator, PlyCount, TimeControl, Time, Termination, Mode, FEN, SetUp, Opening, Variation, Variant, WhiteElo, BlackElo, ECO
};

enum EcoTags {
	eECO, eOpening, eVariation
};

static const char * tags[] = {
	"Unknown", "Event", "Site", "Date", "Round", "White", "Black", "Result",
	"Annotator", "PlyCount", "TimeControl", "Time", "Termination", "Mode", "FEN", "SetUp", "Opening", "Variation", "Variant", "WhiteElo", "BlackElo", "ECO"
};

static const char * ecotags[] = {
	"ECO", "Opening", "Variation"
};

enum Variant {
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
	/// <summary>
	/// Space-separated moves in SAN format without numbers
	/// </summary>
	char sanMoves[MAX_SAN_MOVES_LEN];
	/// <summary>
	/// Chess game PGN header tag array
	/// </summary>
	Tag tags;
	int numberOfPlies;
};

struct EcoLine {
	/// <summary>
	/// Space-separated moves in SAN format without numbers
	/// </summary>
	char sanMoves[MAX_ECO_MOVES_LEN];
	/// <summary>
	/// Chess eco header tag array
	/// </summary>
	EcoTag tags;
};

CHESS_API struct Board * cloneBoard(struct Board * src);
CHESS_API void freeBoard(struct Board * board);
///<summary>
/// Generates unbiased random random number from inclusive range [min, max]
/// The first argument is min, the second is max
///</summary>
CHESS_API int randomNumber(const int, const int);

///<summary>
/// initializes a ZobristHash struct
///</summary>
CHESS_API void zobristHash(struct ZobristHash *);

///<summary>
/// calculates Zobrist hash from a Board struct and updates
/// ZobristHash struct, which should be initialized first
///<summary>
CHESS_API void getHash(struct ZobristHash *, struct Board *);

///<summary>
/// resets ZobristHash struct to initial game position
///</summary>
CHESS_API void resetHash(struct ZobristHash *);

///<summary>
/// updates ZobristHash of a given Board after a given move
///</summary>
CHESS_API int updateHash(struct Board *, struct Move *);

///<summary>
/// fills Square struct from SquareName enum
///</summary>
CHESS_API void square(struct Square *, int squareName);

///<summary>
/// fills ChessPiece struct from a given Square and a PieceName
///</summary>
CHESS_API void piece(struct Square *, struct ChessPiece *, int pieceName);

///<summary>
/// makes a Board struct from a Fen one including legal moves generation stored in Board->movesFromSquares
///</summary>
CHESS_API int fentoboard(struct Fen *, struct Board *);

// four standard bit manupulation functions
CHESS_API unsigned long long bitCount(unsigned long long);
CHESS_API unsigned long lsBit(unsigned long long);
//CHESS_API void unpack_bits(unsigned long long number, float * bit_array);

///<summary>
/// This function strips game result from SAN moves string
///</summary>
CHESS_API void stripGameResult(struct Game *);

///<summary>
/// This function strips comments, variations and NAGs from SAN moves string
///</summary>
CHESS_API int normalizeMoves(char *);

///<summary>
/// This function removes move numbers from normalized SAN moves string
///</summary>
CHESS_API int movesOnly(char *);

///<summary>
/// generates all legal moves on a given board
/// stored in movesFromSquares array of the Board struct
///</summary>
CHESS_API void generateMoves(struct Board *);

///<summary>
/// This function generates board position given an array of enum PieceName[] and its size (the second argument)
/// as well as sideToMove (third argument), castlingRights (fourth argument) and enPassant (fifth argument)
/// The board should be passed by reference in the last argument
///<summary>
CHESS_API void generateEndGame(int * pieceName, int numberOfPieces, int sideToMove, int castlingRights, int enPassant, struct Board *);

CHESS_API int generateEndGames(int maxGameNumber, int maxPieceNumber, char * dataset, char * engine, long long movetime, int depth, int hashSize, int threadNumber, char * syzygyPath, int multiPV, bool logging, bool writedebug, int threads);

///<summary>
/// validates a UCI or SAN move given by the last argument
/// and initializes the Move struct on a given board
///</summary>
CHESS_API int initMove(struct Move * move, struct Board * board, const char * moveString);
CHESS_API void init_move(struct Move * move, struct Board * board, int src, int dst);
CHESS_API bool promoMove(struct Board * board, int src, int dst);

///<summary>
/// makes a given Move on board and updates Board and Fen struct
///</summary>
CHESS_API void makeMove(struct Move * move);
//void undoMove(struct Move * move);

/// <summary>
/// Parses the line into tag name and tag value
/// and fills the given Tag array
/// </summary>
CHESS_API int strtotag(Tag tag, const char * tagString);

/// <summary>
/// Parses the line into tag name and tag value
/// and fills the given EcoTag array
/// </summary>
CHESS_API int strtoecotag(EcoTag, const char * tagString);

///<summary>
/// Count number of games, which begin with a string specified by the second argument
/// from a FILE stream and index them by a game start position in the array long long[].
/// The last argument is the number of games
///</summary>
CHESS_API unsigned long long countGames(FILE *, const char *, unsigned long long gameStartPositions[], unsigned long long maxNumberOfGames);

///<summary>
/// Reads PGN game tags for a first game pointed by a file stream
/// and fills the provided array of typedef Tag
/// returns 0 on success, non-zero on error
///</summary>
CHESS_API int gTags(Tag, FILE *);

///<summary>
/// Reads eco file tags for a first eco line pointed by a file stream
/// and fills the provided array of typedef EcoTag
/// returns 0 on success, non-zero on error
///</summary>
CHESS_API int eTags(EcoTag, FILE *);

///<summary>
/// Plays multiple pgn games from a given pgn file
///</summary>
//unsigned long long openGamesFromPGNfile(char * fileName, int gameThreads, int sqlThreads, char * ecoFileName, int minElo, int maxEloDiff, int minMoves, int numberOfGames, bool generateZobristHash, bool updateDb, bool createDataset, char * dataset, bool eval, char * engine, long long movetime, int depth, int hashSize, int engineThreads, char * syzygyPath, int multiPV, bool logging);

///<summary>
/// This function is similar to playGames() with a difference that it takes a list of PGN
/// files and each thread is given the entire file from the list
/// It saves time by begining to play games one by one from the start of a file
/// eliminating the need to index them in the file first, which is time consuming for large PGN files
/// The first arg is an array of file names, the second arg is the number of of files in this array
/// The rest are the same as in pgnGames()
///</summary>
//unsigned long long openGamesFromPGNfiles(char * fileNames[], int numberOfFiles, int gameThreads, int sqlThreads, char * ecoFileName, int minElo, int numberOfGames, int maxEloDiff, int minMoves, bool generateZobristHash, bool updateDb, bool createDataset, char * dataset, bool eval, char * engine, long long movetime, int depth, int hashSize, int engineThreads, char * syzygyPath, int multiPV, bool logging);

//functions for fast data loading in AI model training
//int initGamesFromPGNs(char * fileNames[], int numberOfFiles, int minElo, int maxEloDiff);
//CHESS_API struct BMPR * dequeueBMPR();
//CHESS_API void * getGame(void * context);
//CHESS_API void * getGameCsv(void * context);
//CHESS_API void getGame_detached(char ** fileNames, const int numberOfFiles, const int minElo, const int maxEloDiff, const int minMoves, const int numberOfChannels, const int numberOfSamples, const int bmprQueueLen, const int gameStage, const unsigned long long steps);
//CHESS_API void free_bmpr(struct BMPR * bmpr);
//CHESS_API int boardLegalMoves(float * boards_legal_moves, int sample, int channels, struct Board * board);
//CHESS_API int getStage(struct Board * board);
//float materialBalance(struct Board * board); //from the view of side to move
CHESS_API void cleanup_magic_bitboards(void);
CHESS_API void init_magic_bitboards(void);

///<summary>
/// This function initializes Game struct from a stream position given by FILE
/// It returns 0 on success and 1 on the EOF
///</summary>
CHESS_API int initGame(struct Game *, FILE *);

///<summary>
/// Plays a game given its struct
///</summary
//int playGame(struct Game *);

/// <summary>
/// draws a chessboard
/// if the second argument is true, then also all legal moves for each piece 
/// </summary>
CHESS_API void writeDebug(struct Board *, bool);

///<summary>
/// draws a board with just a specified piece name such as white pawns, for example
/// filling other squares with '0' or 'o' regardless if it is occupied by other piece or not
///</summary>
//void drawBoard(struct Board *, int pieceName);

///<summary>
/// draws moves from a given square sq
/// it is called from writeDebug
///</summary>
//void drawMoves(struct Board *, int squareName);

///<summary>
/// returns 0 if occupations reconcile with piecesOnSquares,
/// otherwise, non-zero error code
///</summary>
CHESS_API int reconcile(struct Board *);

///<summary>
/// returns string representation in the first argument
/// of a bit field move type given in the second argument
///</summary>
CHESS_API void getMoveType(char *, unsigned int);

///<summary>
/// ECO classificator for a chess game (first argument)
/// second argument - array of ecoLines
/// third argument - the number of ecoLines
///</summary>
CHESS_API void ecoClassify(struct Game *, struct EcoLine **, int);

///<summary>
/// runs a chess engine in a child process
/// The second arg is engine binary path
///</summary>
CHESS_API int engine(struct Engine *, const char *);

CHESS_API struct Engine * initChessEngine(char * engineName, long long movetime, int depth, int hashSize, int threadNumber, char * syzygyPath, int multiPV, bool logging, bool limitStrength, int elo);

CHESS_API void releaseChessEngine(struct Engine * chessEngine);

///<summary>
/// returns chess engine option index for a given name and type
///</summary>
CHESS_API int nametoindex(struct Engine *, const char *, int optionType);

///<summary>
/// gets chess engine options
///</summary>
//int getOptions(char *, struct Engine *);
CHESS_API int getOptions(struct Engine *);

CHESS_API int setOption(struct Engine *, const char *, int optionType, void *);
CHESS_API void setOptions(struct Engine *);

CHESS_API bool isReady(struct Engine *);
CHESS_API bool newGame(struct Engine *);
CHESS_API void stop(struct Engine *);
CHESS_API void quit(struct Engine *);
CHESS_API bool position(struct Engine *);
//void go(long long, int, int, int, char *, bool, bool, long long, long long, long long, long long, int, struct Engine *, struct Evaluation **);
CHESS_API int go(struct Engine *, struct Evaluation **);
CHESS_API float eval(struct Engine *);

//CHESS_API unsigned long long md5(char *);

///<summary>
/// This function returns the number of moves for a given Zobrist hash and 
/// the array of struct MoveScoreGames[MAX_NUMBER_OF_NEXT_MOVES] from NextMovesX.db files
/// where X is the number encoded by the number of most significant bits of the hash
/// The number of bits usually corresponds to sqlThreads (the last arg), which
/// can be 1, 2, 4 or 8. Therefore, X ranges from 0 to 7, i.e. 0 for 1 thread, 0 to 1 for 2 threads,
/// 0 to 3 for 4 threads and 0 to 7 for 8 threads
///</summary>
//CHESS_API int nextMoves(unsigned long long, struct MoveScoreGames **, int);

///<summary>
/// This function is similar to nextMoves except it returns the sorted array of moveScores
/// Sorting is done by weighted absolute scores from the highest to the lowest, 
/// where score = score / totalNumberOfGames for a given position
/// The last arg is the number of sql threads, which should be 1, 2, 4 or 8 depending on how many db files you have
///</summary>
//CHESS_API int bestMoves(unsigned long long, int color, struct MoveScores *, int);

/*
void libchess_init_nnue(const char * nnue_file);
void libchess_init_nnue_context(struct NNUEContext * ctx);
void libchess_free_nnue_context(struct NNUEContext * ctx);
int libchess_evaluate_nnue(const struct Board * board, struct NNUEContext * ctx);
int libchess_evaluate_nnue_incremental(const struct Board * board, const struct Board * prev_board, struct Move * move, struct NNUEContext * ctx);
void libchess_evaluate_dataset(struct Board * boards, struct Move * moves, int * scores, int n_positions);
*/

/*
int openDb(const char *, sqlite3 *);
int closeDb(sqlite3 *);
int getNextMoves(sqlite3 *, sqlite3_int64, struct MoveScoreGames **);
int getNextMove(sqlite3 *, sqlite3_int64, const char *, struct MoveScoreGames *);
int updateNextMove(sqlite3 *, sqlite3_int64, const char *, int);
*/

///<summary>
/// naive chess piece (except pawns) move generator
/// moves are limited by board boundary only
/// may be used at the start to populate move arrays[64] of unsigned long long
///<summary>
//unsigned long long moveGenerator(int pieceType, struct Square *);
//unsigned long long moveGenerator(int pieceType, int squareName);
#ifdef __cplusplus
}
#endif
#endif

