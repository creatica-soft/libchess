#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "nnue/bitboard.h"
#include "libchess.h"

CastlingData CastlingPath[Color_NB][2]; // [Color][0: Kingside, 1: Queenside]

void initCastlingPath(Board& board) {
  for (Color color = ColorWhite; color <= ColorBlack; ++color) {
    //Square kSrc = lsBit(board.side[color] & board.pieceTypes[King - 1]);
    Square kSrc = kingSquare(board, color);
    for (int side = 0; side <= 1; ++side) {
      const uint64_t rooks = board.castlingRooks & board.side[color];
			if (!(rooks)) {
        CastlingPath[color][side].path = 0; // Block if no rights
        continue;
      }
      Square rSrc = side == 0 ? msBit(rooks) : lsBit(rooks);
      Square kDst = (side == 0) ? SQ(baseRank[color], FileG) : SQ(baseRank[color], FileC);
      Square rDst = (side == 0) ? SQ(baseRank[color], FileF) : SQ(baseRank[color], FileD);
			// PATH: Squares that must be EMPTY.
			// We take the squares between King and its destination, 
			// and the squares between Rook and its destination.
			// 1. All squares involved in the King and Rook shuffle
			uint64_t fullSpan = Stockfish::BetweenBB[kSrc][kDst] | Stockfish::BetweenBB[rSrc][rDst];
			// 2. Add destinations ONLY if kSrc==kDst or rSrc==rDst (for 960 "quiet" castling)
			fullSpan |= (SQ_BIT(kDst) | SQ_BIT(rDst));
			// 3. Remove the pieces themselves so they don't block their own path
			CastlingPath[color][side].path = fullSpan & ~(SQ_BIT(kSrc) | SQ_BIT(rSrc));
			// 4. CheckZone is just where the King goes
			CastlingPath[color][side].checkZone = Stockfish::BetweenBB[kSrc][kDst] | SQ_BIT(kDst);
			CastlingPath[color][side].checkZone &= ~SQ_BIT(kSrc);
    }    
  }  
}

// 0xF is 1111 in binary (all 4 rights active)
uint8_t  CastlingRights[64]; 
// 0xFF... is all bits set
uint64_t CastlingRooks[64];  

void initCastlingMasks(Board& board) {
    // Start by assuming every square leaves rights untouched
    for(int i = 0; i < 64; i++) {
        CastlingRights[i] = 0xF; 
        CastlingRooks[i] = ~0ULL;
    }

    // Now, specific squares "kill" specific rights
    for (Color color = ColorWhite; color <= ColorBlack; ++color) {
        Square kSq = kingSquare(board, color);
        // If the King moves, all rights for that color (bits 0,1 or 2,3) are lost
        CastlingRights[kSq] = (color == ColorWhite) ? 0xC : 0x3; 
        CastlingRooks[kSq] &= ~board.side[color]; 

        uint64_t rooks = board.castlingRooks & board.side[color];
        if (!rooks) continue;
        // Kingside (Highest index rook)
        Square rSq = msBit(rooks); 
        // We must ensure the rook we found is actually a Kingside rook.
        // In Standard Chess, this is File H. In 960, we compare to King position.
        if (rSq > kSq) { 
            CastlingRights[rSq] &= ~(CastlingKingside << (static_cast<int>(color) * 2));
            CastlingRooks[rSq] &= ~SQ_BIT(rSq);
        }
        // Queenside (Highest index rook)
        rSq = lsBit(rooks); 
        // We must ensure the rook we found is actually a Queenside rook.
        // In Standard Chess, this is File A. In 960, we compare to King position.
        if (rSq < kSq) { 
            CastlingRights[rSq] &= ~(CastlingQueenside << (static_cast<int>(color) * 2));
            CastlingRooks[rSq] &= ~SQ_BIT(rSq);
        } 
    }
}

//Outermost rook on one side of the king, for X-FEN castling.
//
//X-FEN writes Chess960 castling as plain KQkq, where K means "the OUTERMOST rook on the king's
//side of the board" rather than "the rook on h1". Lichess emits exactly this for 960 games -- a
//live game gives [FEN "rbnqbnkr/... w KQkq - 0 1"], king on g1 -- so a FEN from a real 960 game
//could not be read at all before: KQkq was taken to mean rooks on a1/h1, which is wrong for most
//of the 960 start positions. Measured: 124 of 3840 perft runs failed on the X-FEN encoding of
//the same positions that pass in Shredder notation.
//
//kingside == true asks for the highest-file rook right of the king, false for the lowest-file
//rook left of it. Returns FileNone when there is none.
static unsigned char find_outer_rook_file(const char * rank_str, char rook_char,
                                          unsigned char king_file, bool kingside) {
    unsigned char file = 0;
    unsigned char best = FileNone;
    for (const char * p = rank_str; *p; ++p) {
        const char ch = *p;
        if (isdigit((unsigned char)ch)) { file += (unsigned char)(ch - '0'); continue; }
        if (ch == rook_char && file < 8) {
            if (kingside) { if (file > king_file) best = file; }          //keep the LAST one
            else if (file < king_file && best == FileNone) best = file;   //keep the FIRST one
        }
        file++;
    }
    return best;
}

unsigned char find_king_file(const char * rank_str, char king_char) {
    unsigned char file = 0;
    for (const char *p = rank_str; *p; ++p) {
        char ch = *p;
        if (isdigit(ch)) {
            file += ch - '0';
        } else if (ch == king_char) {
            return file;
        } else {
            file++;
        }
    }
    return FileNone;  // Error value
}

int fen2board(Board& board, const char * fenstr) {
    assert(fenstr);
    size_t len = strlen(fenstr) + 1;
    //assert() is compiled out by -DNDEBUG in BOTH build paths, so it was the only
    //thing standing between an over-long FEN and a stack buffer that strncpy leaves
    //unterminated at exactly this size - strtok_r would then walk off the end.
    //Reject explicitly, and terminate unconditionally.
    if (len > MAX_FEN_STRING_LEN) {
        fprintf(stderr, "fen2board() error: FEN is %zu bytes, maximum is %d\n", len, MAX_FEN_STRING_LEN);
        return 1;
    }

    char local_fen[MAX_FEN_STRING_LEN];
    strncpy(local_fen, fenstr, sizeof(local_fen));
    local_fen[sizeof(local_fen) - 1] = '\0';
    char *saveptr_main = NULL;
    char *token = strtok_r(local_fen, " ", &saveptr_main);
    unsigned char field_idx = 0;

    char *position_ptr = NULL;
    char *castling_ptr = NULL;

    while (token) {
        switch (field_idx) {
        case 0:
            position_ptr = token;
            break;
        case 1:
            if (token[0] == 'w') {
                board.sideToMove = ColorWhite;
            } else if (token[0] == 'b') {
                board.sideToMove = ColorBlack;
            } else {
                fprintf(stderr, "fen2board() error: unable to determine side to move. FEN = %s\n", fenstr);
                return 1;
            }
            break;
        case 2:
            castling_ptr = token;
            if (strlen(castling_ptr) > 4) {
                fprintf(stderr, "fen2board() error: FEN castling field is longer than 4. FEN = %s\n", fenstr);
                return 1;
            }
            break;
        case 3:
            if (token[0] == '-') {
                board.enPassant = FileNone;
            } else {
                board.enPassant = (File)(token[0] - 'a');
            }
            break;
        case 4:
            board.halfmoveClock = atoi(token);
            break;
        case 5:
            board.moveNumber = atoi(token);
            if (board.moveNumber == 0) {
                fprintf(stderr, "fen2board() error: unable to determine move number. FEN = %s\n", fenstr);
                return 1;
            }
            break;
        default:
            fprintf(stderr, "fen2board() error: number of fields in FEN string does not equal 6. FEN = %s\n", fenstr);
            return 1;
        }
        token = strtok_r(NULL, " ", &saveptr_main);
        field_idx++;
    }
    if (field_idx != 6) {
        fprintf(stderr, "fen2board() error: incomplete FEN string. FEN = %s\n", fenstr);
        return 1;
    }

    // Validate position has 7 slashes (8 ranks)
    int slash_count = 0;
    for (const char *p = position_ptr; *p; p++) {
        if (*p == '/') slash_count++;
    }
    if (slash_count != 7) {
        fprintf(stderr, "fen2board() error: incorrect number of ranks in position field. FEN = %s\n", fenstr);
        return 1;
    }

    // Castling parsing
    //((unsigned int *)board.castlingRook)[0] = 0x08080808;
    board.castlingRooks = 0;
    board.castlingRights = 0;
    //These were never reset, so every cached flag leaked from the PREVIOUS position:
    //one Chess960 game left isChess960 true for the life of the process, and a stale
    //isCheck made nnue.cpp return the NNUE_CHECK sentinel for a position not in check.
    board.isChess960 = false;
    board.isCheck = false;
    board.isMate = false;
    board.isStaleMate = false;
    board.num_moves = 0;

    if (castling_ptr[0] != '-') {
        const char wf[] = "ABCDEFGH";
        const char bf[] = "abcdefgh";
        const char std[] = "KQkq";
        unsigned char white_king_file = FileNone;
        unsigned char black_king_file = FileNone;
        int f;
        char rank_buf[9];

        //Rank 1 and rank 8 as written, needed to locate the king and the rooks for X-FEN.
        //Extracted once, up front, rather than lazily inside each branch as before.
        char rank1_buf[9] = {0}, rank8_buf[9] = {0};
        {
            const char * last_slash = strrchr(position_ptr, '/');
            const char * rank8_end  = strchr(position_ptr, '/');
            if (!last_slash || !rank8_end) {
                fprintf(stderr, "fen2board() error: malformed position field. FEN = %s\n", fenstr);
                return 1;
            }
            size_t l1 = strlen(last_slash + 1), l8 = (size_t)(rank8_end - position_ptr);
            if (l1 > 8 || l8 > 8) {
                fprintf(stderr, "fen2board() error: rank size greater than 8. FEN = %s\n", fenstr);
                return 1;
            }
            memcpy(rank1_buf, last_slash + 1, l1);
            memcpy(rank8_buf, position_ptr, l8);
        }

        for (size_t c = 0; castling_ptr[c] != '\0'; c++) {
            char ch = castling_ptr[c];
            if (strchr(std, ch)) {
                //X-FEN: KQkq names the OUTERMOST rook on that side of the king, which is a1/h1
                //only when the position is the standard one. Infer it from the board instead of
                //assuming. For standard chess this yields exactly a1/h1/a8/h8 and nothing changes.
                const bool white = (ch == 'K' || ch == 'Q');
                const bool kside = (ch == 'K' || ch == 'k');
                unsigned char& kf = white ? white_king_file : black_king_file;
                if (kf == FileNone) {
                    kf = find_king_file(white ? rank1_buf : rank8_buf, white ? 'K' : 'k');
                    if (kf == FileNone) {
                        fprintf(stderr, "fen2board() error: %s king not found in rank. FEN = %s\n",
                                white ? "white" : "black", fenstr);
                        return 1;
                    }
                }
                const unsigned char rf = find_outer_rook_file(white ? rank1_buf : rank8_buf,
                                                              white ? 'R' : 'r', kf, kside);
                if (rf == FileNone) {
                    //A castling right with no rook to exercise it. Drop it rather than inventing
                    //a rook on a1/h1, which is what the old code effectively did.
                    continue;
                }
                board.castlingRooks |= SQ_BIT(SQ(white ? Rank1 : Rank8, static_cast<File>(rf)));
                board.castlingRights |= kside ? (CastlingKingside << (white ? 0 : 2))
                                              : (CastlingQueenside << (white ? 0 : 2));
                //Only a genuinely non-standard placement makes this Chess960. Getting this wrong
                //in either direction matters: isChess960 selects the castling MOVE ENCODING
                //(king-takes-rook vs king-to-g1), so a standard game must not set it.
                const unsigned char stdKing = FileE;
                const unsigned char stdRook = kside ? FileH : FileA;
                if (kf != stdKing || rf != stdRook) board.isChess960 = true;
            } else if (strchr(wf, ch)) {
                board.isChess960 = true;
                if (white_king_file == FileNone) {
                    const char *last_slash = strrchr(position_ptr, '/');
                    if (!last_slash) {
                        fprintf(stderr, "fen2board() error: malformed position field. FEN = %s\n", fenstr);
                        return 1;
                    }
                    const char *rank1_start = last_slash + 1;
                    size_t rank_len = strlen(rank1_start);
                    if (rank_len > 8) {
                        fprintf(stderr, "fen2board() error: rank size greater than 8. FEN = %s\n", fenstr);
                        return 1;
                    }
                    strncpy(rank_buf, rank1_start, rank_len);
                    rank_buf[rank_len] = '\0';
                    white_king_file = find_king_file(rank_buf, 'K');
                    if (white_king_file == FileNone) {
                        fprintf(stderr, "fen2board() error: white king not found in rank. FEN = %s\n", fenstr);
                        return 1;
                    }
                }
                f = tolower(ch) - 'a';
                if ((unsigned char)f > white_king_file) {
                    //board.castlingRook[0][0] = static_cast<File>(f);
                    board.castlingRooks |= SQ_BIT(SQ(Rank1, static_cast<File>(f)));
                    board.castlingRights |= CastlingKingside;
                } else {
                    //board.castlingRook[0][1] = static_cast<File>(f);
                    board.castlingRooks |= SQ_BIT(SQ(Rank1, static_cast<File>(f)));
                    board.castlingRights |= CastlingQueenside;
                }
            } else if (strchr(bf, ch)) {
                board.isChess960 = true;
                if (black_king_file == FileNone) {
                    const char *rank8_end = strchr(position_ptr, '/');
                    if (!rank8_end) {
                        fprintf(stderr, "fen2board() error: malformed position field. FEN = %s\n", fenstr);
                        return 1;
                    }
                    size_t rank_len = rank8_end - position_ptr;
                    if (rank_len > 8) {
                        fprintf(stderr, "fen2board() error: rank size greater than 8. FEN = %s\n", fenstr);
                        return 1;
                    }
                    strncpy(rank_buf, position_ptr, rank_len);
                    rank_buf[rank_len] = '\0';
                    black_king_file = find_king_file(rank_buf, 'k');
                    if (black_king_file == FileNone) {
                        fprintf(stderr, "fen2board() error: black king not found in rank. FEN = %s\n", fenstr);
                        return 1;
                    }
                }
                f = ch - 'a';
                if ((unsigned char)f > black_king_file) {
                    //board.castlingRook[1][0] = static_cast<File>(f);
                    board.castlingRooks |= SQ_BIT(SQ(Rank8, static_cast<File>(f)));
                    board.castlingRights |= (CastlingKingside << 2);
                } else {
                    //board.castlingRook[1][1] = static_cast<File>(f);
                    board.castlingRooks |= SQ_BIT(SQ(Rank8, static_cast<File>(f)));
                    board.castlingRights |= (CastlingQueenside << 2);
                }
            }
        }
    }
    char *saveptr_ranks = NULL;
    char *rank_token = strtok_r(position_ptr, "/", &saveptr_ranks);
    unsigned char rank_idx = 7;

    memset(board.side, 0, sizeof board.side);
    memset(board.pieceTypes, 0, sizeof board.pieceTypes);
    while (rank_token) {
        size_t token_len = strlen(rank_token);
        if (token_len > 8) {
            fprintf(stderr, "fen2board() error: rank size greater than 8. FEN = %s\n", fenstr);
            return 1;
        }
        unsigned char file_idx = 0;
        for (const char *ptr = rank_token; *ptr; ptr++) {
            char ch = *ptr;
            if (isdigit(ch)) {
                unsigned char skip = ch - '0';
                for (unsigned char k = 0; k < skip; k++) {
                    if (file_idx >= 8) {
                        fprintf(stderr, "fen2board() error: rank overflows 8 squares. FEN = %s\n", fenstr);
                        return 1;
                    }
                    unsigned char sq = SQ(static_cast<Rank>(rank_idx), static_cast<File>(file_idx));
                    board.piecesOnSquares[sq] = PieceNone;
                    file_idx++;
                }
            } else {
                const char symbols[] = "PNBRQKpnbrqk";
                const char * found = strchr(symbols, ch);
                if (found) {
                    int s = found - symbols;
                    if (s >= 0 && s <= 11) {
                        if (file_idx >= 8) {
                            fprintf(stderr, "fen2board() error: rank overflows 8 squares. FEN = %s\n", fenstr);
                            return 1;
                        }
                        Square sq = SQ(static_cast<Rank>(rank_idx), static_cast<File>(file_idx));
                        unsigned long long bitsq = (1ULL << sq);
                        if (s < 6) board.side[ColorWhite] |= bitsq;
                        else board.side[ColorBlack] |= bitsq;                          
                        board.pieceTypes[s % 6] |= bitsq;
                        board.piecesOnSquares[sq] = s < 6 ? (Piece)(s + 1) : (Piece)(s + 3);
                        file_idx++;
                    } //else if (s == 0) {
                        //file_idx++;
                    //}
                } else {
                    fprintf(stderr, "fen2board() error: invalid character in FEN %s: %c\n", fenstr, ch);
                    return 1;
                }
            }
        }
        if (file_idx != 8) {
            fprintf(stderr, "fen2board() error: rank does not sum to 8 squares. FEN = %s\n", fenstr);
            return 1;
        }
        rank_token = strtok_r(NULL, "/", &saveptr_ranks);
        if (rank_idx-- == 0) break;
    }
    if (rank_idx != 255 || rank_token != NULL) {
        fprintf(stderr, "fen2board() error: incorrect number of ranks. FEN = %s\n", fenstr);
        return 1;
    }
    initCastlingPath(board);
    initCastlingMasks(board);
    return 0;
}
