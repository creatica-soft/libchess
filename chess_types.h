// chess_types.h -- the SINGLE definition of struct Board and of the enums it needs.
//
// Included by BOTH sides of the libchess / vendored-Stockfish seam:
//   * libchess.h    -- the project's own public header
//   * nnue/board.h  -- a forwarder, kept under that name so the ten
//                      #include "../board.h" / "../../board.h" lines inside the
//                      vendored tree stay byte-identical and re-syncable.
//
// KEEP THIS HEADER MINIMAL. Nothing else from libchess.h may migrate in here:
//
//  * NOT the FILE_A..FILE_H bitboard macros (libchess.h). They collide head-on
//    with Stockfish's enum File { FILE_A.. } in nnue/types.h and silently
//    miscompile edge_distance() in nnue/bitboard.h -- measured: it returns the
//    wrong distance for files E..H, with no warning.
//  * NOT the global enum operator templates (operator++/--/+/- in
//    libchess.h). They are constrained only by std::is_enum, so they apply to
//    every Stockfish enum as well as libchess's own, which is why nnue/ must
//    not see them. The three that were outright broken have been repaired:
//    operator&(T,int) ignored its second argument and always masked with 1 and
//    is now GONE entirely (mask with a plain int, or use PC_TYPE/PC_COLOR);
//    postfix operator++ incremented and then assigned the old value back, so it
//    left its operand unchanged, and now returns the old value by value; and
//    operator-= was declared to return int while assigning to a T&, so it did
//    not compile when instantiated. None of the three had a live call site.
//  * NOT noise.h, the MAX_* macros, the char* name tables, or the SQ_*/PC_*
//    helpers. None of them is referenced from nnue/.
//
// WHY THE TWO Piece ENCODINGS CAN BE static_cast INTO ONE ANOTHER
//   libchess:  PieceWhite=0, WhitePawn=1..WhiteKing=6, PieceNone=7,
//              PieceBlack=8, BlackPawn=9..BlackKing=14
//   Stockfish: NO_PIECE=0,   W_PAWN=1..W_KING=6,       (7 unused),
//              (8 unused),   B_PAWN=9..B_KING=14
// The twelve real pieces coincide exactly; the encodings DISAGREE at 0, 7 and 8.
// That is benign only because every Stockfish table indexed by Piece happens to
// be harmless at exactly those indices -- PieceValue[] (nnue/types.h) is
// VALUE_ZERO at 0/7/8/15 and HalfKAv2_hm::PieceSquareIndex is PS_NONE there --
// and because every read of piecesOnSquares[] inside nnue/ is driven by an
// occupancy bitboard, so a sentinel never reaches an index. This is luck, not
// design: if upstream ever renumbers Piece, this is what breaks.
//
// PieceNone MUST stay == 7. nnue/nnue/nnue_misc.cpp writes and compares it.

#ifndef CHESS_TYPES_H
#define CHESS_TYPES_H

#include <cstddef>
#include <cstdint>

enum Color : uint8_t { ColorWhite, ColorBlack, Color_NB };

enum File : uint8_t {FileA, FileB, FileC, FileD, FileE, FileF, FileG, FileH, FileNone, File_NB = 8};

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

enum PieceType : uint8_t { PieceTypeAny, Pawn, Knight, Bishop, Rook, Queen, King, PieceTypeNone, PieceType_NB = 6, NonPawnType_NB = 5 };
//enum PieceType : uint8_t { PieceTypeNone, Pawn, Knight, Bishop, Rook, Queen, King, PieceTypeAny, PieceType_NB = 6, NonPawnType_NB = 5 };

// Piece enumeration: first three bits are used to encode the type, fourth bit defines the color, total 16 pieces
// Shifting Piece by 3 to the right gives PieceColor: color = piece >> 3
// Masking 3 lowest bits returns the PieceType: type = piece & 7
// PieceNone is exception to the above rules
// PieceNone has color white and type PieceTypeNone
enum Piece : uint8_t {
	PieceWhite, WhitePawn, WhiteKnight, WhiteBishop, WhiteRook, WhiteQueen, WhiteKing, PieceNone,
	//PieceNone, WhitePawn, WhiteKnight, WhiteBishop, WhiteRook, WhiteQueen, WhiteKing, PieceWhite,
	PieceBlack, BlackPawn, BlackKnight, BlackBishop, BlackRook, BlackQueen, BlackKing, Piece_NB = 12, NonPawn_NB = 10
};

struct Board {
    //these booleans are not strictly needed except maybe isChess960
    bool isCheck = false;
    bool isStaleMate = false;
    bool isMate = false;
    bool isChess960 = false;
    Color sideToMove = ColorWhite;
    File enPassant = FileNone; //a FILE index, not a square; FileNone == 8 means "no en passant"
    uint8_t halfmoveClock = 0;
    uint8_t num_moves = 0; //not necessary but good for 8-byte alignment on 64-bit systems
    //8 bytes up to here
    int moveNumber = 1;
    uint8_t castlingRights = 0xf;
    //3 bytes of padding follow; they are NOT zeroed even by `Board b{}`.
    //Never memcmp() or raw-hash a Board.
    //the rest is aligned on 8-byte boundary
    uint64_t castlingRooks = 0; //8 bytes
    //all 64 entries are spelled out on purpose. `= {PieceNone}` sets only
    //element 0 and value-initialises the other 63 to Piece(0) == PieceWhite,
    //which is neither a piece nor "empty": it slips past reconcile()'s
    //`pn != PieceNone` guard in board.cpp and indexes pieceTypes[-1].
    Piece piecesOnSquares[Square_NB] = { //64 bytes
        PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone,
        PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone,
        PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone,
        PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone,
        PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone,
        PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone,
        PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone,
        PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone, PieceNone
    };
    uint64_t side[Color_NB] = {0}; //all white and all black - 16 bytes
    uint64_t pieceTypes[PieceType_NB] = {0}; //all pawns, knights, bishops, rooks, queens, kings - 48 bytes
    //total 152 bytes
};

// ---------------------------------------------------------------------------
// Layout contract.
//
// The three marked PERMANENT are not transitional: nnue/nnue/nnue_accumulator.cpp
// does *reinterpret_cast<const std::array<Piece, 64>*>(board.piecesOnSquares),
// which silently corrupts the accumulator refresh cache if that member ever
// moves or changes element size. Keep them forever.
//
// The remaining offsetof lines are the TRANSITIONAL net for the deduplication
// refactor: they pin the layout that the two former definitions of Board
// shared, so a slip while unifying them fails the build instead of
// miscompiling. They may be deleted once the single definition has shipped and
// been stable -- but only those; the three permanent ones stay.
// ---------------------------------------------------------------------------
// PERMANENT, and deliberately outside the 64-bit guard below: these two are
// platform-independent and they are what protects
//   *reinterpret_cast<const std::array<Piece, 64>*>(board.piecesOnSquares)
// in nnue/nnue/nnue_accumulator.cpp (two sites in update_accumulator_refresh_cache).
// Upstream's own static_assert(sizeof(Piece) == 1) in that file sits inside
// #if defined(USE_AVX512) || defined(USE_AVX2), so it is INACTIVE on ARM --
// do not delete these believing upstream covers them.
static_assert(sizeof(Piece) == 1, "PERMANENT: nnue_accumulator.cpp reinterpret_casts piecesOnSquares to std::array<Stockfish::Piece, 64>");
static_assert(sizeof(Board::piecesOnSquares) == 64, "PERMANENT: nnue_accumulator.cpp reinterpret_casts piecesOnSquares to std::array<Stockfish::Piece, 64>");

#if defined(UINTPTR_MAX) && UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFULL   //the numbers below are the 64-bit layout
static_assert(sizeof(Board) == 152, "PERMANENT: Board layout changed");
static_assert(alignof(Board) == 8, "Board alignment changed");
static_assert(offsetof(Board, isCheck) == 0, "Board layout changed");
static_assert(offsetof(Board, isStaleMate) == 1, "Board layout changed");
static_assert(offsetof(Board, isMate) == 2, "Board layout changed");
static_assert(offsetof(Board, isChess960) == 3, "Board layout changed");
static_assert(offsetof(Board, sideToMove) == 4, "Board layout changed");
static_assert(offsetof(Board, enPassant) == 5, "Board layout changed");
static_assert(offsetof(Board, halfmoveClock) == 6, "Board layout changed");
static_assert(offsetof(Board, num_moves) == 7, "Board layout changed");
static_assert(offsetof(Board, moveNumber) == 8, "Board layout changed");
static_assert(offsetof(Board, castlingRights) == 12, "Board layout changed");
static_assert(offsetof(Board, castlingRooks) == 16, "Board layout changed");
static_assert(offsetof(Board, piecesOnSquares) == 24, "PERMANENT: Board layout changed");
static_assert(offsetof(Board, side) == 88, "Board layout changed");
static_assert(offsetof(Board, pieceTypes) == 104, "Board layout changed");
#endif

#endif //CHESS_TYPES_H
