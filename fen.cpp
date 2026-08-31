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
    assert(len <= MAX_FEN_STRING_LEN);

    char local_fen[MAX_FEN_STRING_LEN];
    strncpy(local_fen, fenstr, sizeof(local_fen));
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
    //board.isChess960 = false;

    if (castling_ptr[0] != '-') {
        const char wf[] = "ABCDEFGH";
        const char bf[] = "abcdefgh";
        const char std[] = "KQkq";
        unsigned char white_king_file = FileNone;
        unsigned char black_king_file = FileNone;
        int f;
        char rank_buf[9];

        for (size_t c = 0; castling_ptr[c] != '\0'; c++) {
            char ch = castling_ptr[c];
            if (strchr(std, ch)) {
                switch (ch) {
                case 'K':
                    //board.castlingRook[0][0] = FileH;
                    board.castlingRooks |= SQ_BIT(SquareH1);
                    board.castlingRights |= CastlingKingside;
                    break;
                case 'Q':
                    //board.castlingRook[0][1] = FileA;
                    board.castlingRooks |= SQ_BIT(SquareA1);
                    board.castlingRights |= CastlingQueenside;
                    break;
                case 'k':
                    //board.castlingRook[1][0] = FileH;
                    board.castlingRooks |= SQ_BIT(SquareH8);
                    board.castlingRights |= (CastlingKingside << 2);
                    break;
                case 'q':
                    //board.castlingRook[1][1] = FileA;
                    board.castlingRooks |= SQ_BIT(SquareA8);
                    board.castlingRights |= (CastlingQueenside << 2);
                    break;
                }
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
