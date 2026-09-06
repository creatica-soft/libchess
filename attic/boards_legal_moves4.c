//This file is used to experiment with data and data loading to train chess AI models from PGN and CSV files
#pragma warning(disable:4996)
#include <omp.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
//#include <time.h>
//#include <math.h>
//#include <pthread.h> //use -pthread when compiling and linking
//#include "uthash.h"
//#include "magic_bitboards.h"
#include "libchess.h"

// this function attemps to prioritise moves of major pieces attacked by minor pieces,
// captures of major pieces by minor pieces or capture of undefended pieces and
// avoid moves to squares attacked by minor pieces
float moveValue(struct Board * board, enum SquareName src, enum SquareName dst) {
  unsigned long bitBoard;
  enum SquareName s;
  enum PieceType pt = board->piecesOnSquares[src] & 7;
  int shiftedColor = board->opponentColor << 3;
  if (pt < King) {
    for (enum PieceType p = Queen; p > pt; p--) {
      bitBoard = board->occupations[shiftedColor | p];
      while (bitBoard) { //for all opponent pieces that greater than pt
        s = lsBit(bitBoard);
        if (dst == s) return 1.0; //prioritise capture of major pieces
        bitBoard &= bitBoard - 1;
      }
    }
    for (enum PieceType p = pt; p >= Pawn; p--) { 
      bitBoard = board->occupations[shiftedColor | p];
      while (bitBoard) { //for all opponent pieces that greater than pt
        s = lsBit(bitBoard);
        if (dst == s && !(board->defendedPieces & s))
          return 1.0; //prioritise capture of underfended pieces equal or lower than pt
        bitBoard &= bitBoard - 1;
      }
    }
    for (enum PieceType p = Pawn; p < pt; p++) { 
      bitBoard = board->occupations[shiftedColor | p];
      while (bitBoard) { //for all opponent pieces that smaller than pt
        s = lsBit(bitBoard);
        if ((board->movesFromSquares[s] & src) && !(board->movesFromSquares[s] & dst))
          return 1.0; //prioritise moves for pieces attacked by a minor piece (included pinned one)
        if (board->movesFromSquares[s] & dst)
          return 0.001; //avoid moves to squares attacked by a minor piece even if it's pinned
        bitBoard &= bitBoard - 1;
      }
    }
  }
  return 0.1; //default move value to contrast it with priority moves (1.0) and moves to avoid (0.001)
}

//this function fills BMPR structure member - an array of boards_legal_moves
int boardLegalMoves(float * boards_legal_moves, int sample, int channels, struct Board * board) {
	unsigned char channel = 0;
  unsigned long bitBoard, bitBoard2;
	int offset;
	int sampleXchannels = sample * channels * 64;
	enum SquareName src, dst;
	if (!boards_legal_moves) {
	  printf("boardLegalMoves(%d) error: boards_legal_moves is NULL\n", omp_get_thread_num());
	  return -1;	  
	}
  //sanity check - should be commented out later
  /*
  if ((board->fen->sideToMove == board->opponentColor) ||  board->opponentColor > ColorBlack || board->opponentColor < ColorWhite) {
    fprintf(stderr, "boardLegalMoves() error: opponentColor is either the same as sideToMove or greater than 1 or smaller than 0\n");
    exit(-1);
  }
  */
  
	//white occupation bitboards - 6 channels from 0 (pawns) to 5 (king)
  for (enum PieceName pn = WhitePawn; pn <= WhiteKing; pn++) { //white pieces
    //printf("pieceName %s occupation %lu\n", pieceName[pn], board->occupations[pn]);
    offset = sampleXchannels + channel * 64;
    bitBoard = board->occupations[pn];
    while (bitBoard) {
      src = lsBit(bitBoard);
      boards_legal_moves[offset + src] = 1.0;
      bitBoard &= bitBoard - 1;
    }
    channel++;
  }
  
	//black occupation bitboards - 6 channels from 6 (pawns) to 11 (king) 
  for (enum PieceName pn = BlackPawn; pn <= BlackKing; pn++) { //black pieces
    //printf("pieceName %s occupation %lu\n", pieceName[pn], board->occupations[pn]);
    offset = sampleXchannels + channel * 64;
    bitBoard = board->occupations[pn];
    while (bitBoard) {
      src = lsBit(bitBoard);
      boards_legal_moves[offset + src] = 1.0;
      bitBoard &= bitBoard - 1;      
    }
    channel++;
  }

  //all white piece values (for material balance) - channel 12
  //printf("pieceName %s occupation %lu\n", pieceName[PieceNameWhite], board->occupations[PieceNameWhite]);
  offset = sampleXchannels + channel * 64;
  bitBoard = board->occupations[PieceNameWhite];
  while (bitBoard) {
    src = lsBit(bitBoard);
    boards_legal_moves[offset + src] = pieceValue[board->piecesOnSquares[src] & 7];
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //all black piece values (for material balance) - channel 13
  //printf("pieceName %s occupation %lu\n", pieceName[PieceNameBlack], board->occupations[PieceNameBlack]);
  offset = sampleXchannels + channel * 64;
  bitBoard = board->occupations[PieceNameBlack];
  while (bitBoard) {
    src = lsBit(bitBoard);
    boards_legal_moves[offset + src] = pieceValue[board->piecesOnSquares[src] & 7];
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //opponent's controlled squares: channels 14 to 29 (16 channels - one for each piece)
  int shiftedColor = board->opponentColor << 3;
  unsigned char unusedChannels[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  unsigned char idx = 0, num = 0;
  unsigned long pawns = board->occupations[shiftedColor | Pawn];
  for (enum Files f = FileA; f <= FileH; f++) {
    bitBoard = pawns & bitFiles[f]; //pawn occupations on a file
    if (bitBoard) offset = sampleXchannels + channel * 64;
    else unusedChannels[idx++] = channel;
    while (bitBoard) {
      src = lsBit(bitBoard);
      bitBoard2 = board->movesFromSquares[src];
      while (bitBoard2) {
        dst = lsBit(bitBoard2);
        boards_legal_moves[offset + dst] = 1.0;
        bitBoard2 &= bitBoard2 - 1;    
      }
      bitBoard &= bitBoard - 1;
    }
    channel++;
  }
  
  for (enum PieceType pt = Knight; pt <= Rook; pt++) {
    bitBoard = board->occupations[shiftedColor | pt];
    num = __builtin_popcountl(bitBoard);
    if (num <= 1) num = 2;
    for (int i = 0; i < num; i++) {
      if (i >= 2) offset = sampleXchannels + unusedChannels[--idx] * 64;
      else {
        offset = sampleXchannels + channel * 64;
        channel++;
      }
      if (bitBoard) {
        src = lsBit(bitBoard);
        bitBoard &= bitBoard - 1;
        bitBoard2 = board->movesFromSquares[src];
        while (bitBoard2) {
          dst = lsBit(bitBoard2);
          boards_legal_moves[offset + dst] = 1.0;
          bitBoard2 &= bitBoard2 - 1;    
        }
      }
    }
  }
  
  bitBoard = board->occupations[shiftedColor | Queen];
  while (bitBoard) {
    num = __builtin_popcountl(bitBoard);
    if (num < 2) offset = sampleXchannels + channel * 64;
    else offset = sampleXchannels + unusedChannels[--idx] * 64;
    src = lsBit(bitBoard);
    bitBoard2 = board->movesFromSquares[src];
    while (bitBoard2) {
      dst = lsBit(bitBoard2);
      boards_legal_moves[offset + dst] = 1.0;
      bitBoard2 &= bitBoard2 - 1;    
    }
    bitBoard &= bitBoard - 1;
  }
  channel++;

  bitBoard = board->occupations[shiftedColor | King];
  offset = sampleXchannels + channel * 64;
  channel++;
  src = lsBit(bitBoard);
  bitBoard2 = board->movesFromSquares[src];
  while (bitBoard2) {
    dst = lsBit(bitBoard2);
    boards_legal_moves[offset + dst] = 1.0;
    bitBoard2 &= bitBoard2 - 1;    
  }
  
  //legal moves 16 channels 30 to 45 (total 46 channels)
  shiftedColor = board->fen->sideToMove << 3;
  bzero(unusedChannels, 8);
  idx = 0;
  pawns = board->occupations[shiftedColor | Pawn];
  for (enum Files f = FileA; f <= FileH; f++) {
    bitBoard = pawns & bitFiles[f]; //pawn occupations on a file
    if (bitBoard) offset = sampleXchannels + channel * 64;
    else unusedChannels[idx++] = channel;
    while (bitBoard) {
      src = lsBit(bitBoard);
      bitBoard2 = board->movesFromSquares[src];
      while (bitBoard2) {
        dst = lsBit(bitBoard2);
        boards_legal_moves[offset + dst] = moveValue(board, src, dst);
        bitBoard2 &= bitBoard2 - 1;    
      }
      bitBoard &= bitBoard - 1;
    }
    channel++;
  }
  for (enum PieceType pt = Knight; pt <= Rook; pt++) {
    bitBoard = board->occupations[shiftedColor | pt];
    num = __builtin_popcountl(bitBoard);
    if (num <= 1) num = 2;
    for (int i = 0; i < num; i++) {
      if (i >= 2) offset = sampleXchannels + unusedChannels[--idx] * 64;
      else {
        offset = sampleXchannels + channel * 64;
        channel++;
      }
      if (bitBoard) {
        src = lsBit(bitBoard);
        bitBoard &= bitBoard - 1;
        bitBoard2 = board->movesFromSquares[src];
        while (bitBoard2) {
          dst = lsBit(bitBoard2);
          boards_legal_moves[offset + dst] = moveValue(board, src, dst);
          bitBoard2 &= bitBoard2 - 1;    
        }
      }
    }
  }
  
  bitBoard = board->occupations[shiftedColor | Queen];
  while (bitBoard) {
    num = __builtin_popcountl(bitBoard);
    if (num < 2) offset = sampleXchannels + channel * 64;
    else offset = sampleXchannels + unusedChannels[--idx] * 64;
    src = lsBit(bitBoard);
    bitBoard2 = board->movesFromSquares[src];
    while (bitBoard2) {
      dst = lsBit(bitBoard2);
      boards_legal_moves[offset + dst] = moveValue(board, src, dst);
      bitBoard2 &= bitBoard2 - 1;    
    }
    bitBoard &= bitBoard - 1;
  }
  channel++;

  bitBoard = board->occupations[shiftedColor | King];
  offset = sampleXchannels + channel * 64;
  channel++;
  src = lsBit(bitBoard);
  bitBoard2 = board->movesFromSquares[src];
  while (bitBoard2) {
    dst = lsBit(bitBoard2);
    boards_legal_moves[offset + dst] = moveValue(board, src, dst);
    bitBoard2 &= bitBoard2 - 1;    
  }
  
  //legal moves for masking model predictions: channels 46 to 119 (total 110 channels)
  for (src = SquareA1; src <= SquareH8; src++) {
    offset = sampleXchannels + channel * 64;
	  //printf("legal moves from square %s %lu\n", squareName[sq], board->sideToMoveMoves[sq]);    
    bitBoard = board->sideToMoveMoves[src];
    while (bitBoard) {
      dst = lsBit(bitBoard);
      boards_legal_moves[offset + dst] = 1.0;
      bitBoard &= bitBoard - 1;    
    }
    channel++;
  } 


  //sanity check, could be commented out once verified
  if (channel != channels) {
	  printf("boardLegalMoves(%d) error: number of channels (%d) != channels (%d)\n", omp_get_thread_num(), channel, channels);
	  return -2;
	}
	
	return 0;
}
