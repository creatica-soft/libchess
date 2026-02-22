#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "libchess.h"

//#ifdef __cplusplus
//extern "C" {
//#endif

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
    ((unsigned int *)board.castlingRook)[0] = 0x08080808;
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
                    board.castlingRook[0][0] = FileH;
                    break;
                case 'Q':
                    board.castlingRook[0][1] = FileA;
                    break;
                case 'k':
                    board.castlingRook[1][0] = FileH;
                    break;
                case 'q':
                    board.castlingRook[1][1] = FileA;
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
                    board.castlingRook[0][0] = f;
                } else {
                    board.castlingRook[0][1] = f;
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
                    board.castlingRook[1][0] = f;
                } else {
                    board.castlingRook[1][1] = f;
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
                    unsigned char sq = SQ(rank_idx, file_idx);
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
                        Square sq = SQ(rank_idx, file_idx);
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

    return 0;
}

//#ifdef __cplusplus
//}
//#endif