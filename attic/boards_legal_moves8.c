//This file is used to experiment with data and data loading to train chess AI models from PGN and CSV files
#pragma warning(disable:4996)
#include <omp.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

#ifdef __cplusplus
extern "C" {
#endif

//this function fills BMPR structure member - an array of boards_legal_moves
int boardLegalMoves(float * boards_legal_moves, int sample, int channels, struct Board * board) {
	unsigned char channel = 0;
  unsigned long bitBoard, bitBoard2;
	int offset; //a piece occupation offset in boards_legal_moves array
	int offset2; //control square offset in boards_legal_moves array
	int offset3; // used for legal moves source squares
	int sampleXchannels = sample * channels * 64; //sample offset in boards_legal_moves array
	int src, dst;

  //each channel is a chessboard 8 x 8 (64 squares)
  //Channels 0 - 5: 6 channels for opponents pawns, knights, bishops, rooks, queens and king
  //Channels 6 - 11: 6 channels for sideToMove pawns, knights, bishops, rooks, queens and king
  //Channels 12 - 17: 6 channels for opponent's control squares and 
  //Channel 18: source squares for legal moves
  //Channels 19 - 24: 6 channels for move types 19 pawns, 20 knights, 21 bishops, 22 rooks, 23 queens, 24 king
  //Channels 25 - 28: 4 channels for promotions to Q (25), R (26), B (27), K (28) - total 29 channels
  //promotion channels are just for promotion moves, i.e. e8 for white or a1 for black
  //total 29 channels
  //only 19 used as inputs, the last 10 are for legal mask in training loop or for inference to mask illegal moves
  for (int c = 0; c < 2; c++) { //loop over two colors
    int color = c == 0 ? board->opponentColor : board->fen->sideToMove;
    unsigned char shiftedColor = color << 3;
    int pawn = shiftedColor | Pawn;
    int king = shiftedColor | King;
    for (int pn = pawn; pn <= king; pn++) { //loop over different piece types of the same color
      bitBoard = board->occupations[pn];
      if (bitBoard) {
        offset = sampleXchannels + channel * 64;
        if (color == board->fen->sideToMove) offset2 = offset + 64 * 13;
        else offset2 = offset + 64 * 12;
      }
      while (bitBoard) { //loop over occupations of the same piece types of the same color
        src = lsBit(bitBoard);
        boards_legal_moves[offset + src] = 1.0;
        bitBoard2 = board->movesFromSquares[src];
        while (bitBoard2) { //loop over all moves from src square
          dst = lsBit(bitBoard2);
          if (color == board->fen->sideToMove) {
            int move_type = (pn == pawn && dst / 8 == (color == ColorWhite ? 7 : 0)) ? 6 : (pn & 7) - 1; 
            if (move_type == 6) { //promo move
              // Promotions: Set channels 25-28 (6 + 13 + 6..9 = 25..28)
              for (int promo = 6; promo <= 9; promo++)
                boards_legal_moves[offset2 + 64 * promo + dst] = 1.0;
            } else boards_legal_moves[offset2 + dst] = 1.0;
            board->channel[src] = move_type; // Map to move_type (0–6)
            board->sourceSquare[move_type] = src; //reversed map
            //printf("boardLegalMoves(): move type for %s %s\n", color == 0 ? "white" : "black", pieceType[move_type + 1]);
          } else boards_legal_moves[offset2 + dst] = 1.0; //opponent's control squares
          bitBoard2 &= bitBoard2 - 1;    
        }
        bitBoard &= bitBoard - 1;
      } //end of while bitBoard loop over occupations of the same piece types for a given color
      channel++;
    } //end of for piece name loop (same color)
  } //end of for color loop
  offset3 = sampleXchannels + 64 * 18; // channel 18 is for legal move source squares
  for (int sn = SquareA1; sn <= SquareH8; sn++) {
    if (board->sideToMoveMoves[sn]) boards_legal_moves[offset3 + sn] = 1.0;
  }
  	
	return 0;
}
#ifdef __cplusplus
}
#endif
