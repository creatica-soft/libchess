#pragma warning(disable:4996)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"


//see https://en.wikipedia.org/wiki/Forsyth%E2%80%93Edwards_Notation

//X-FEN implementation with features relevant to standard or chess 960 (such as castling rights and en passant), 10x8 board and extra pieces such as A or C are not supported
/// <summary>
///  Converts FEN string to struct Fen
/// </summary>
/// <return> 
/// 0 for success, non-zero for failure
/// </return>
int strtofen(struct Fen * fen, const char * fenstr) {
	size_t count = strlen(fenstr) + 1;
	if (count > sizeof fen->fenString) {
		printf("strtofen() error: FEN string is too long (%zu), it must be shorter than %zu.\n", count, sizeof fen->fenString);
		return 1;
	}
	strncpy(fen->fenString, fenstr, count);
	char * fenString = strdup(fenstr);
	if (!fenString) {
		printf("Error in strtofen(): strdup(%s) returned null - %s\n", fenstr, strerror(errno));
		goto err;
	}
	//fprintf(stderr, "strtofen(): fen %s\n", fenstr);

	char position[73];
	char castling[5];
	char * saveptr;
	char * token = strtok_r(fenString, " ", &saveptr);
	unsigned char i = 0;
	size_t sz;

	while (token) {
		switch (i) {
		case 0:
			sz = strlen(token);
			if (sz < sizeof(position))
				strncpy(position, token, sz + 1);
			else {
				printf("Error in strtofen(): FEN position field is longer than %zu. FEN = %s\n", sz, fenString);
				goto err;
			}
			break;
		case 1:
			if (strncmp(token, "w", 1) == 0)
				fen->sideToMove = ColorWhite;
			else if (strncmp(token, "b", 1) == 0)
				fen->sideToMove = ColorBlack;
			else {
				printf("Error in strtofen(): unable to determine side to move. FEN = %s\n", fenString);
				goto err;
			}
			break;
		case 2:
			sz = strlen(token);
			if (sz < sizeof(castling))
				strncpy(castling, token, sz + 1);
			else {
				printf("Error in strtofen(): FEN castling field is longer than %zu. FEN = %s\n", sz, fenString);
				goto err;
			}
			break;
		case 3:
			//in X-FEN spec en passant is only set if pseudo legal capture is possible;
			//another words, when an opposite color pawn is on the same rank and on the adjacent to en passant file
			fen->enPassantLegalBit = 0;
			if (strncmp(token, "-", 1) == 0)
				fen->enPassant = FileNone;
			else
				fen->enPassant = token[0] - 'a';
			break;
		case 4:
			fen->halfmoveClock = atoi(token);
			break;
		case 5:
			fen->moveNumber = atoi(token);
			if (fen->moveNumber == 0) {
				printf("Error in strtofen(): unable to determine move number. FEN = %s\n", fenString);
				goto err;
			}
			break;
		default:
			printf("Error in strtofen(): number of fields in FEN string does not equal 6. FEN = %s\n", fenString);
			goto err;
			break;
		}
		token = strtok_r(NULL, " ", &saveptr);
		i++;
	}
	token = strtok_r(position, "/", &saveptr);
	i = 7;
	while (token) {
		sz = strlen(token);
		if (sz > 8) {
			printf("Error in strtofen(): rank size greater than 8, fenString %s\n", fenString);
			goto err;
		}
		strncpy(fen->ranks[i], token, sz + 1);
		if (i == 0) break;
		token = strtok_r(NULL, "/", &saveptr);
		i--;
	}
	fen->castlingRook[0][0] = FileNone;
	fen->castlingRook[0][1] = FileNone;
	fen->castlingRook[1][0] = FileNone;
	fen->castlingRook[1][1] = FileNone;
	fen->castlingRights = CastlingRightsWhiteNoneBlackNone;
	fen->castlingBits = 0;

	if (strncmp(castling, "-", 1) != 0) {
		char wf[] = "ABCDEFGH";
		char bf[] = "abcdefgh";
		char r[] = "KQkq";
		enum Files f;
		unsigned char white_king_file = 0;
		unsigned char black_king_file = 0;

		for (unsigned char c = 0; c < 4; c++) {
			if (castling[c] == '\0') break;
			if (memchr(r, castling[c], 4)) {
				fen->isChess960 = false;
				switch (castling[c]) {
				case 'K':
					fen->castlingRook[0][0] = FileH;
					fen->castlingBits |= (1UL << FileH);
					fen->castlingRights |= CastlingSideKingside;
					break;
				case 'Q':
					fen->castlingRook[1][0] = FileA;
					fen->castlingBits |= (1UL << FileA);
					fen->castlingRights |= CastlingSideQueenside;
					break;
				case 'k':
					fen->castlingRook[0][1] = FileH;
					fen->castlingBits |= (1UL << (FileH + 56));
					fen->castlingRights |= (CastlingSideKingside << 2);
					break;
				case 'q':
					fen->castlingRook[1][1] = FileA;
					fen->castlingBits |= (1UL << (FileA + 56));
					fen->castlingRights |= (CastlingSideQueenside << 2);
					break;
				}
			} else if (memchr(wf, castling[c], 8)) {
				fen->isChess960 = true;
				//to find out whether the castling kingside or queenside for chess 960
				//we need to find on which side of the king is the castling rook;
				//hence, first we need to find where the king is
				if (white_king_file == 0) {
					for (unsigned char ch = FileA; ch <= FileH; ch++) {
						if (isdigit(fen->ranks[0][ch])) white_king_file += ch;
						else if (ch == 'K') break;
						else white_king_file++;
					}
				}
				f = (enum Files)(tolower(c) - 'a');
				if ((unsigned char)f > white_king_file) {
					fen->castlingRook[0][0] = f;
					fen->castlingBits |= (1UL << f);
					fen->castlingRights |= CastlingSideKingside;
				} else {
					fen->castlingRook[1][0] = f;
					fen->castlingBits |= (1UL << f);
					fen->castlingRights |= CastlingSideQueenside;
				}
			} else if (memchr(bf, castling[c], 8)) {
				fen->isChess960 = true;
				if (black_king_file == 0) {
					for (unsigned char ch = 0; ch <= 7; ch++) {
						if (isdigit(fen->ranks[7][ch])) black_king_file += ch;
						else if (ch == 'k') break;
						else black_king_file++;
					}
				}
				f = c - 'a';
				if ((unsigned char)f > black_king_file) {
					fen->castlingRook[0][1] = f;
					fen->castlingBits |= (1UL << (f + 56));
					fen->castlingRights |= (CastlingSideKingside << 2);
				} else {
					fen->castlingRook[1][1] = f;
					fen->castlingBits |= (1UL << (f + 56));
					fen->castlingRights |= (CastlingSideQueenside << 2);
				}
			}
		}
	}
	free(fenString);
	return 0;
err:
	if (fenString) free(fenString);
	return 1;
}

/// <summary>
///  Converts Fen struct to FEN string
/// </summary>
int fentostr(struct Fen * fen) {	
	char castling[5];
	if (fen->castlingRights == CastlingRightsWhiteNoneBlackNone) {
		castling[0] = '-';
		castling[1] = '\0';
	}
	else {
		unsigned char i = 0;
		for (enum Color color = ColorWhite; color <= ColorBlack; color++)
		{
			if (fen->isChess960)
			{
				if (((fen->castlingRights >> (color << 1)) & CastlingSideKingside) == CastlingSideKingside) {
					castling[i++] = color == ColorWhite ? toupper(fen->castlingRook[0][0] + 'a') : fen->castlingRook[0][1] + 'a';
				}
				if (((fen->castlingRights >> (color << 1)) & CastlingSideQueenside) == CastlingSideQueenside) {
					castling[i++] = color == ColorWhite ? toupper(fen->castlingRook[1][0] + 'a') : fen->castlingRook[1][1] + 'a';
				}
			}
			else
			{
				if (((fen->castlingRights >> (color << 1)) & CastlingSideKingside) == CastlingSideKingside) {
					castling[i++] = color == ColorWhite ? 'K' : 'k';
				}
				if (((fen->castlingRights >> (color << 1)) & CastlingSideQueenside) == CastlingSideQueenside) {
					castling[i++] = color == ColorWhite ? 'Q' : 'q';
				}
			}
		}
		castling[i] = '\0';
	}
	char en_passant[3];
	if (fen->enPassant == FileNone) {
		en_passant[0] = '-';
		en_passant[1] = '\0';
	}
	else {
		en_passant[0] = fen->enPassant + 'a';
		en_passant[1] = fen->sideToMove == ColorWhite ? '6' : '3';
		en_passant[2] = '\0';
	}

	return sprintf(fen->fenString, "%s/%s/%s/%s/%s/%s/%s/%s %c %s %s %u %u", fen->ranks[7], fen->ranks[6], fen->ranks[5], fen->ranks[4], fen->ranks[3], fen->ranks[2], fen->ranks[1], fen->ranks[0], fen->sideToMove == ColorWhite ? 'w' : 'b', castling, en_passant, fen->halfmoveClock, fen->moveNumber);
}

