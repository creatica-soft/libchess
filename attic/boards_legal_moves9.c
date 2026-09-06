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
	int channel = 0, offset, sq;
  unsigned long bitBoard;
	int sampleXchannels = sample * channels * 64;

  //each channel is a chessboard 8 x 8 (64 squares)
  //total 87 channels
  
  //Channels 0 - 5: 6 channels for opponents pawns, knights, bishops, rooks, queens and king
  //Channels 6 - 11: 6 channels for sideToMove pawns, knights, bishops, rooks, queens and king
  for (int c = 0; c < 2; c++) { //loop over two colors starting with the opponent's color
    int color = c == 0 ? board->opponentColor : board->fen->sideToMove;
    unsigned char shiftedColor = color << 3;
    int pawn = shiftedColor | Pawn;
    int king = shiftedColor | King;
    for (int pn = pawn; pn <= king; pn++) { //loop over different piece types of the same color
      bitBoard = board->occupations[pn];
      if (bitBoard) offset = sampleXchannels + channel * 64;
      while (bitBoard) { //loop over occupations of the same piece types of the same color
        sq = lsBit(bitBoard);
        boards_legal_moves[offset + sq] = 1.0;
        bitBoard &= bitBoard - 1;
      } //end of while bitBoard loop over occupations of the same piece types for a given color
      channel++;
    } //end of for piece name loop (same color)
  } //end of for color loop

  //Channel 12: all opponent pieces (board->occupations[(board->opponentColor << 3) | PieceTypeAny])
  //Channel 13: all sideToMove pieces (board->occupations[(board->fen->sideToMove << 3) | PieceTypeAny])
  //Channel 14: opponent's defended pieces (board->defendedPieces)
  //Channel 15: sideToMove pieces attacked by opponent (board->attackedPieces)
  //Channel 16: all empty squares controlled by opponent (board->attackedSquares & board->occupations[PieceNameNone])
  //Channel 17: checkers (board->checkers)
  //Channel 18: check blocking squares (board->blockingSquares)
  //Channel 19: pinned pieces (board->pinnedPieces)
  //Channel 20: pinning pieces (board->pinningPieces)
  //Channel 21: en passant legal square calculated by isEnPassantLegal(); (board->fen->enPassantLegalBit)
  //Channel 22: //rook squares if castling with that rook is allowed by FEN (board->fen->castlingBits)
  for (int i = 0; i < 11; i++) {
    if (i == 0) bitBoard = board->occupations[(board->opponentColor << 3) | PieceTypeAny];
    else if (i == 1) bitBoard = board->occupations[(board->fen->sideToMove << 3) | PieceTypeAny];
    else if (i == 2) bitBoard = board->defendedPieces;
    else if (i == 3) bitBoard = board->attackedPieces;
    else if (i == 4) bitBoard = board->attackedSquares & board->occupations[PieceNameNone];
    else if (i == 5) bitBoard = board->checkers;
    else if (i == 6) bitBoard = board->blockingSquares;
    else if (i == 7) bitBoard = board->pinnedPieces;
    else if (i == 8) bitBoard = board->pinningPieces;
    else if (i == 9) bitBoard = board->fen->enPassantLegalBit;
    else if (i == 10) bitBoard = board->fen->castlingBits;
    offset = sampleXchannels + channel * 64;
    while (bitBoard) { //loop over occupations of the same piece types of the same color
      sq = lsBit(bitBoard);
      boards_legal_moves[offset + sq] = 1.0;
      bitBoard &= bitBoard - 1;
    } //end of while bitBoard loop over occupations of the same piece types for a given color
    channel++;
  }
  //Channels 23 - 86: destination squares for legal moves
  offset = sampleXchannels + channel * 64;
  for (int sn = SquareA1; sn <= SquareH8; sn++) {
    bitBoard = board->sideToMoveMoves[sn];
    if (bitBoard) offset = sampleXchannels + channel * 64;
    while (bitBoard) {
      sq = lsBit(bitBoard);
      boards_legal_moves[offset + sq] = 1.0;
      bitBoard &= bitBoard - 1;      
    }
    channel++;
  }
  if (channel != channels) {
    fprintf(stderr, "boardLegalMoves() error: channel %d != channels %d\n", channel, channels);
    exit(1);
  }
  
	return 0;
}
#ifdef __cplusplus
}
#endif
