#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "libchess.h"

#ifdef __cplusplus
extern "C" {
#endif

unsigned char find_king_file(const char *rank_str, char king_char) {
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

int strtofen(struct Fen * fen, const char * fenstr) {
    /*if (!fen) {
        fprintf(stderr, "strtofen() error: fen arg is NULL\n");
        return 1;
    }
    if (len > MAX_FEN_STRING_LEN) {
        fprintf(stderr, "strtofen() error: FEN string is too long (%zu), it must be shorter than %d\n", len, MAX_FEN_STRING_LEN);
        return 1;
    }*/
    assert(fen);
    size_t len = strlen(fenstr) + 1;
    assert(len <= MAX_FEN_STRING_LEN);
    strncpy(fen->fenString, fenstr, MAX_FEN_STRING_LEN);

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
                fen->sideToMove = ColorWhite;
            } else if (token[0] == 'b') {
                fen->sideToMove = ColorBlack;
            } else {
                fprintf(stderr, "strtofen() error: unable to determine side to move. FEN = %s\n", fenstr);
                return 1;
            }
            break;
        case 2:
            castling_ptr = token;
            if (strlen(castling_ptr) > 4) {
                fprintf(stderr, "strtofen() error: FEN castling field is longer than 4. FEN = %s\n", fenstr);
                return 1;
            }
            break;
        case 3:
            if (token[0] == '-') {
                fen->enPassant = FileNone;
            } else {
                fen->enPassant = token[0] - 'a';
            }
            //fen->enPassantLegalBit = 0;
            break;
        case 4:
            fen->halfmoveClock = atoi(token);
            break;
        case 5:
            fen->moveNumber = atoi(token);
            if (fen->moveNumber == 0) {
                fprintf(stderr, "strtofen() error: unable to determine move number. FEN = %s\n", fenstr);
                return 1;
            }
            break;
        default:
            fprintf(stderr, "strtofen() error: number of fields in FEN string does not equal 6. FEN = %s\n", fenstr);
            return 1;
        }
        token = strtok_r(NULL, " ", &saveptr_main);
        field_idx++;
    }
    if (field_idx != 6) {
        fprintf(stderr, "strtofen() error: incomplete FEN string. FEN = %s\n", fenstr);
        return 1;
    }

    // Validate position has 7 slashes (8 ranks)
    int slash_count = 0;
    for (const char *p = position_ptr; *p; p++) {
        if (*p == '/') slash_count++;
    }
    if (slash_count != 7) {
        fprintf(stderr, "strtofen() error: incorrect number of ranks in position field. FEN = %s\n", fenstr);
        return 1;
    }

    // Castling parsing
    memset(fen->castlingRook, FileNone, sizeof(fen->castlingRook));
    fen->castlingRights = CastlingRightsWhiteNoneBlackNone;
    //fen->castlingBits = 0;
    fen->isChess960 = false;

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
                    fen->castlingRook[0][0] = FileH;
                    //fen->castlingBits |= (1ULL << FileH);
                    fen->castlingRights |= CastlingSideKingside;
                    break;
                case 'Q':
                    fen->castlingRook[1][0] = FileA;
                    //fen->castlingBits |= (1ULL << FileA);
                    fen->castlingRights |= CastlingSideQueenside;
                    break;
                case 'k':
                    fen->castlingRook[0][1] = FileH;
                    //fen->castlingBits |= (1ULL << (FileH + 56));
                    fen->castlingRights |= (CastlingSideKingside << 2);
                    break;
                case 'q':
                    fen->castlingRook[1][1] = FileA;
                    //fen->castlingBits |= (1ULL << (FileA + 56));
                    fen->castlingRights |= (CastlingSideQueenside << 2);
                    break;
                }
            } else if (strchr(wf, ch)) {
                fen->isChess960 = true;
                if (white_king_file == FileNone) {
                    const char *last_slash = strrchr(position_ptr, '/');
                    if (!last_slash) {
                        fprintf(stderr, "strtofen() error: malformed position field. FEN = %s\n", fenstr);
                        return 1;
                    }
                    const char *rank1_start = last_slash + 1;
                    size_t rank_len = strlen(rank1_start);
                    if (rank_len > 8) {
                        fprintf(stderr, "strtofen() error: rank size greater than 8. FEN = %s\n", fenstr);
                        return 1;
                    }
                    strncpy(rank_buf, rank1_start, rank_len);
                    rank_buf[rank_len] = '\0';
                    white_king_file = find_king_file(rank_buf, 'K');
                    if (white_king_file == FileNone) {
                        fprintf(stderr, "strtofen() error: white king not found in rank. FEN = %s\n", fenstr);
                        return 1;
                    }
                }
                f = tolower(ch) - 'a';
                if ((unsigned char)f > white_king_file) {
                    fen->castlingRook[0][0] = f;
                    //fen->castlingBits |= (1ULL << f);
                    fen->castlingRights |= CastlingSideKingside;
                } else {
                    fen->castlingRook[1][0] = f;
                    //fen->castlingBits |= (1ULL << f);
                    fen->castlingRights |= CastlingSideQueenside;
                }
            } else if (strchr(bf, ch)) {
                fen->isChess960 = true;
                if (black_king_file == FileNone) {
                    const char *rank8_end = strchr(position_ptr, '/');
                    if (!rank8_end) {
                        fprintf(stderr, "strtofen() error: malformed position field. FEN = %s\n", fenstr);
                        return 1;
                    }
                    size_t rank_len = rank8_end - position_ptr;
                    if (rank_len > 8) {
                        fprintf(stderr, "strtofen() error: rank size greater than 8. FEN = %s\n", fenstr);
                        return 1;
                    }
                    strncpy(rank_buf, position_ptr, rank_len);
                    rank_buf[rank_len] = '\0';
                    black_king_file = find_king_file(rank_buf, 'k');
                    if (black_king_file == FileNone) {
                        fprintf(stderr, "strtofen() error: black king not found in rank. FEN = %s\n", fenstr);
                        return 1;
                    }
                }
                f = ch - 'a';
                if ((unsigned char)f > black_king_file) {
                    fen->castlingRook[0][1] = f;
                    //fen->castlingBits |= (1ULL << (f + 56));
                    fen->castlingRights |= (CastlingSideKingside << 2);
                } else {
                    fen->castlingRook[1][1] = f;
                    //fen->castlingBits |= (1ULL << (f + 56));
                    fen->castlingRights |= (CastlingSideQueenside << 2);
                }
            }
        }
    }
    return 0;
}

int fentoboard(struct Fen *fen, struct Board *board) {
    if (!fen || !board) {
        fprintf(stderr, "fentoboard() error: either fen or board or both args are NULL\n");
        return 1;
    }
    struct ZobristHash *zh = board->zh;
    memset(board, 0, sizeof(struct Board));
    board->zh = zh;

    char local_fen[MAX_FEN_STRING_LEN];
    strncpy(local_fen, fen->fenString, sizeof(local_fen));
    char *saveptr_main = NULL;
    char *position = strtok_r(local_fen, " ", &saveptr_main);
    if (!position) {
        fprintf(stderr, "fentoboard() error: invalid FEN string format. FEN = %s\n", fen->fenString);
        return 1;
    }

    char position_copy[73];
    strncpy(position_copy, position, sizeof(position_copy));
    char *saveptr_ranks = NULL;
    char *rank_token = strtok_r(position_copy, "/", &saveptr_ranks);
    unsigned char rank_idx = 7;

    while (rank_token) {
        size_t token_len = strlen(rank_token);
        if (token_len > 8) {
            fprintf(stderr, "fentoboard() error: rank size greater than 8. FEN = %s\n", fen->fenString);
            return 1;
        }
        unsigned char file_idx = 0;
        for (const char *ptr = rank_token; *ptr; ptr++) {
            char ch = *ptr;
            if (isdigit(ch)) {
                unsigned char skip = ch - '0';
                for (unsigned char k = 0; k < skip; k++) {
                    if (file_idx >= 8) {
                        fprintf(stderr, "fentoboard() error: rank overflows 8 squares. FEN = %s\n", fen->fenString);
                        return 1;
                    }
                    unsigned char sq = SQ(rank_idx, file_idx);
                    board->piecesOnSquares[sq] = PieceNameNone;
                    board->occupations[PieceNameNone] |= SQ_BIT(sq);
                    file_idx++;
                }
            } else {
                const char symbols[] = ".PNBRQK..pnbrqk";
                const char *found = strchr(symbols, ch);
                if (found) {
                    char s = found - symbols;
                    if (s > 0 && s < 15) {
                        if (file_idx >= 8) {
                            fprintf(stderr, "fentoboard() error: rank overflows 8 squares. FEN = %s\n", fen->fenString);
                            return 1;
                        }
                        unsigned char sq = SQ(rank_idx, file_idx);
                        board->occupations[(int)s] |= SQ_BIT(sq);
                        board->piecesOnSquares[sq] = (int)s;
                        file_idx++;
                    } else if (s == 0) {
                        file_idx++;
                    }
                } else {
                    fprintf(stderr, "fentoboard() error: invalid character in FEN %s: %c\n", fen->fenString, ch);
                    return 1;
                }
            }
        }
        if (file_idx != 8) {
            fprintf(stderr, "fentoboard() error: rank does not sum to 8 squares. FEN = %s\n", fen->fenString);
            return 1;
        }
        rank_token = strtok_r(NULL, "/", &saveptr_ranks);
        if (rank_idx-- == 0) break;
    }
    if (rank_idx != 255 || rank_token != NULL) {
        fprintf(stderr, "fentoboard() error: incorrect number of ranks. FEN = %s\n", fen->fenString);
        return 1;
    }

    board->occupations[PieceNameWhite] = board->occupations[WhiteBishop] | board->occupations[WhiteKing] | board->occupations[WhiteKnight] | board->occupations[WhitePawn] | board->occupations[WhiteQueen] | board->occupations[WhiteRook];
    board->occupations[PieceNameBlack] = board->occupations[BlackBishop] | board->occupations[BlackKing] | board->occupations[BlackKnight] | board->occupations[BlackPawn] | board->occupations[BlackQueen] | board->occupations[BlackRook];
    board->occupations[PieceNameAny] = board->occupations[PieceNameWhite] | board->occupations[PieceNameBlack];
    board->fen = fen;
    generateMoves(board);
    return 0;
}

int fentostr(struct Fen *fen) {
    fprintf(stderr, "fentostr() error: deprecated - use updateFen with a board instead.\n");
    return -1;
}

#ifdef __cplusplus
}
#endif