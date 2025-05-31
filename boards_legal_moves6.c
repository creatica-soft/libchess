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
  enum PieceType pt = board->piecesOnSquares[src] & 7; //piece type to move
  int shiftedColor = board->opponentColor << 3;
  if (pt == King) return 0.5;
  for (enum PieceType p = Queen; p > pt; p--) {
      if (board->occupations[shiftedColor | p] & (1UL << dst)) return 0.5 + pieceValue[p]; //try to prioritise capture of superior pieces
  }
  for (enum PieceType p = pt; p >= Pawn; p--) { 
      if ((board->occupations[shiftedColor | p] & (1UL << dst)) && !(board->defendedPieces & (1UL << dst)))
        return 0.5 + pieceValue[p]; //try to prioritise capture of underfended pieces equal or lower than pt
  }
  for (enum PieceType p = Pawn; p < pt; p++) { 
    bitBoard = board->occupations[shiftedColor | p];
    while (bitBoard) { //for all opponent pieces that smaller than pt
      s = __builtin_ctzl(bitBoard);
      if ((board->movesFromSquares[s] & (1UL << src)) && !(board->movesFromSquares[s] & (1UL << dst)))
        return 0.75; //try to prioritise moves for pieces attacked by a minor piece (included pinned one)
      if (board->movesFromSquares[s] & (1UL << dst))
        return 0.25; //try to avoid moves to squares attacked by a minor piece even if it's pinned
      bitBoard &= bitBoard - 1;
    }
  }
  return 0.5; //default move value to contrast it with priority moves (1.0) and moves to avoid (0.001)
}

//this function fills BMPR structure member - an array of boards_legal_moves
int boardLegalMoves(float * boards_legal_moves, int sample, int channels, struct Board * board) {
	unsigned char channel = 0;
  unsigned long bitBoard, bitBoard2;
	int offset; //a piece occupation offset in boards_legal_moves array
	int offset2; //a move or control square offset in boards_legal_moves array
	int sampleXchannels = sample * channels * 64; //sample offset in boards_legal_moves array
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
  
  //white or black pieces: 16 channels (one for each piece) and
  //white or black controlled squares or moves: (16 channels - one for each piece)
  //8 channels for white pawns: one per file; if there are no pawns, the channel is kept for promotions
  //plus another 8 channels for white pawns either all moves or control squares (diag moves)
  for (enum Color color = ColorWhite; color <= ColorBlack; color++) {
    unsigned int shiftedColor = color << 3;
    unsigned long pawns = board->occupations[shiftedColor | Pawn];
    for (enum Files f = FileA; f <= FileH; f++) {
      bitBoard = pawns & bitFiles[f]; //pawn occupations on a file
      if (bitBoard) {
        offset = sampleXchannels + channel * 64; 
        offset2 = offset + 64;
      } 
      while (bitBoard) { //loop over all white pawns
        src = __builtin_ctzl(bitBoard); //src square for a pawn
        boards_legal_moves[offset + src] = 1.0;
        bitBoard2 = board->movesFromSquares[src]; //all moves or control squares from src square
        while (bitBoard2) { //loop over pawn moves (for opponent these are just diagonal pawn moves)
          dst = __builtin_ctzl(bitBoard2);
          boards_legal_moves[offset2 + dst] = board->fen->sideToMove == color ? moveValue(board, src, dst) : 1.0;
          bitBoard2 &= bitBoard2 - 1;    
        }
        bitBoard &= bitBoard - 1;
      }
      channel += 2;
    }
    //6 channels for pairs of white knights, bishops and rooks
    //plus another 6 channels for either their moves or control squares
    //if there are more than a pair (very rare), we add extra piece(s) to the same channel(s)
    enum PieceName knight = shiftedColor | Knight;
    enum PieceName rook = shiftedColor | Rook;
    for (enum PieceName pn = knight; pn <= rook; pn++) {
      bitBoard = board->occupations[pn];
      for (int i = 0; i < 2; i++) {
        offset = sampleXchannels + channel * 64;
        offset2 = offset + 64;
        if (bitBoard) {
          src = __builtin_ctzl(bitBoard);
          boards_legal_moves[offset + src] = 1.0;
          bitBoard2 = board->movesFromSquares[src];
          while (bitBoard2) {
            dst = __builtin_ctzl(bitBoard2);
            boards_legal_moves[offset2 + dst] = board->fen->sideToMove == color ? moveValue(board, src, dst) : 1.0;
            bitBoard2 &= bitBoard2 - 1;    
          }
          bitBoard &= bitBoard - 1;
          if (bitBoard && i == 1) {
            //fprintf(stderr, "Warning: Extra %s detected, merging into channel %d\n", pieceName[pn], channel);
            while (bitBoard) { // Handle extra pieces (e.g., underpromotions)
              src = __builtin_ctzl(bitBoard);
              boards_legal_moves[offset + src] = 1.0;
              bitBoard2 = board->movesFromSquares[src];
              while (bitBoard2) {
                dst = __builtin_ctzl(bitBoard2);
                boards_legal_moves[offset2 + dst] = (board->fen->sideToMove == color) ? moveValue(board, src, dst) : 1.0;
                bitBoard2 &= bitBoard2 - 1;
              }
              bitBoard &= bitBoard - 1;
            }
          }
        }
        channel += 2;
      }
    }
    //1 channel for white or black queen
    //plus another channel for its moves or control squares
    //if there are more than one queen, we add them to the same channel
    bitBoard = board->occupations[shiftedColor | Queen];
    if (bitBoard) {
      offset = sampleXchannels + channel * 64;
      offset2 = offset + 64;
    }
    while (bitBoard) {
      src = __builtin_ctzl(bitBoard);
      boards_legal_moves[offset + src] = 1.0;
      bitBoard2 = board->movesFromSquares[src];
      while (bitBoard2) {
        dst = __builtin_ctzl(bitBoard2);
        boards_legal_moves[offset2 + dst] = board->fen->sideToMove == color ? moveValue(board, src, dst) : 1.0;
        bitBoard2 &= bitBoard2 - 1;    
      }
      bitBoard &= bitBoard - 1;
      /*if (bitBoard) {
        fprintf(stderr, "Warning: Extra %s detected, merging into queen channel\n", pieceName[shiftedColor | Queen]);
      }*/
    }
    channel += 2;
    
    //one channel for white king occupation, plus another one for its moves or control squares
    bitBoard = board->occupations[shiftedColor | King];
    offset = sampleXchannels + channel * 64;
    offset2 = offset + 64;
    channel += 2;
    src = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + src] = 1.0;
    bitBoard2 = board->movesFromSquares[src];
    while (bitBoard2) {
      dst = __builtin_ctzl(bitBoard2);
      boards_legal_moves[offset2 + dst] = board->fen->sideToMove == color ? moveValue(board, src, dst) : 1.0;
      bitBoard2 &= bitBoard2 - 1;    
    }
  }

  //legal moves for masking model predictions: channels 64 to 127 (total 128 channels)
  for (src = SquareA1; src <= SquareH8; src++) {
    offset = sampleXchannels + channel * 64;
	  //printf("legal moves from square %s %lu\n", squareName[sq], board->sideToMoveMoves[sq]);    
    bitBoard = board->sideToMoveMoves[src];
    while (bitBoard) {
      dst = __builtin_ctzl(bitBoard);
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
