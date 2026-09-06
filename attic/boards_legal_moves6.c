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

// this function attempts to prioritise moves of major pieces attacked by minor pieces,
// captures of major pieces by minor pieces or capture of undefended pieces and
// avoid moves to squares attacked by minor pieces
// somehow this prioritisation does not work!
// perhaps, another idea to try: instead of non-binary moveValues, mark pieces that under attack or undefended
// use four different channels:
// 1. attacked superior opponent's pieces defended or not
// 2. attacked undefended opponent's pieces
// 3. pieces attacked by opponent's inferior pieces
// 4. dst squares controlled by opponent's inferior pieces
/*
float moveValue(struct Board * board, enum SquareName src, enum SquareName dst) {
  unsigned long bitBoard;
  enum SquareName s;
  enum PieceType pt = board->piecesOnSquares[src] & 7; //piece type to move
  int shiftedColor = board->opponentColor << 3;
  if (pt == King) return 0.1;
  for (enum PieceType p = Queen; p > pt; p--) {
      if (board->occupations[shiftedColor | p] & (1UL << dst)) return 1.0; //try to prioritise capture of superior pieces
  }
  for (enum PieceType p = pt; p >= Pawn; p--) { 
      if ((board->occupations[shiftedColor | p] & (1UL << dst)) && !(board->defendedPieces & (1UL << dst)))
        return 1.0; //try to prioritise capture of undefended pieces equal or lower than pt
  }
  for (enum PieceType p = Pawn; p < pt; p++) { 
    bitBoard = board->occupations[shiftedColor | p];
    while (bitBoard) { //for all opponent pieces that inferior than pt
      s = lsBit(bitBoard);
      if ((board->movesFromSquares[s] & (1UL << src)) && !(board->movesFromSquares[s] & (1UL << dst)))
        return 1.0; //try to prioritise moves for pieces attacked by an inferior piece (included pinned one)
      if (board->movesFromSquares[s] & (1UL << dst))
        return 0.0; //try to avoid moves to squares attacked by an inferior piece even if it's pinned
      bitBoard &= bitBoard - 1;
    }
  }
  return 0.1; //default move value to contrast it with priority moves (1.0) and moves to avoid (0.0)
}
*/

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
  //each channel is a chessboard 8 x 8 (64 squares)
  //white or black pieces: 16 channels (one for each piece) and
  //white or black controlled squares or moves: (16 channels - one for each piece)
  unsigned char sideToMove = board->fen->sideToMove << 3;
  unsigned char oppositeSide = board->opponentColor << 3;
  unsigned long knights = board->occupations[sideToMove | Knight], 
                bishops = board->occupations[sideToMove | Bishop], 
                rooks = board->occupations[sideToMove | Rook], 
                queens = board->occupations[sideToMove | Queen], 
                all = board->occupations[sideToMove | PieceTypeAny], 
                oKnights = board->occupations[oppositeSide | Knight],
                oBishops = board->occupations[oppositeSide | Bishop],
                oRooks = board->occupations[oppositeSide | Rook],
                oQueens = board->occupations[oppositeSide | Queen],
                oAll = board->occupations[oppositeSide | PieceTypeAny], 
                oUndefended = (oAll ^ board->defendedPieces) & oAll, 
                attackedSuperior = 0, 
                attackedUndefended = 0, 
                attackedByInferior = 0, 
                controlledByInferior = 0;

  for (enum Color color = ColorWhite; color <= ColorBlack; color++) {
    unsigned char shiftedColor = color << 3;
    //8 channels for white or black pawns and 8 channels for their moves or control squares (diag moves)
    unsigned long pawns = board->occupations[shiftedColor | Pawn];
    for (enum Files f = FileA; f <= FileH; f++) {
      bitBoard = pawns & bitFiles[f]; //pawn occupations on a file
      if (bitBoard) {
        offset = sampleXchannels + channel * 64; 
        offset2 = offset + 64;
      } 
      while (bitBoard) { //loop over all white or black pawns
        src = lsBit(bitBoard); //src square for a pawn
        boards_legal_moves[offset + src] = 1.0;
        bitBoard2 = board->movesFromSquares[src]; //all moves or control squares from src square
        if (board->fen->sideToMove == color) {
          attackedSuperior |= bitBoard2 & (oKnights | oBishops | oRooks | oQueens);
          attackedUndefended |= bitBoard2 & oUndefended;
        } else {
          attackedByInferior |= bitBoard2 & (knights | bishops | rooks | queens);
        }
        while (bitBoard2) { //loop over pawn moves (for opponent these are just diagonal pawn moves)
          dst = lsBit(bitBoard2);
          boards_legal_moves[offset2 + dst] = 1.0; //board->fen->sideToMove == color ? moveValue(board, src, dst) : 1.0;
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
        if (bitBoard) {
          offset = sampleXchannels + channel * 64;
          offset2 = offset + 64;
          src = lsBit(bitBoard);
          boards_legal_moves[offset + src] = 1.0;
          bitBoard2 = board->movesFromSquares[src];
          if (board->fen->sideToMove == color) {
            if (pn < rook) {
              attackedSuperior |= bitBoard2 & (oRooks | oQueens);
              controlledByInferior |= bitBoard2 & board->oPawnMoves;
            } else {
              attackedSuperior |= bitBoard2 & oQueens;
              controlledByInferior |= bitBoard2 & (board->oPawnMoves | board->oKnightMoves | board->oBishopMoves);
            }
            attackedUndefended |= bitBoard2 & oUndefended;
          } else {
            if (pn < rook) attackedByInferior |= bitBoard2 & (rooks | queens);
            else attackedByInferior |= bitBoard2 & queens;
          }
          while (bitBoard2) {
            dst = lsBit(bitBoard2);
            boards_legal_moves[offset2 + dst] = 1.0; //board->fen->sideToMove == color ? moveValue(board, src, dst) : 1.0;
            bitBoard2 &= bitBoard2 - 1;    
          }
          bitBoard &= bitBoard - 1;
          if (bitBoard && i == 1) {
            //fprintf(stderr, "Warning: Extra %s detected, merging into channel %d\n", pieceName[pn], channel);
            while (bitBoard) { // Handle extra pieces (e.g., underpromotions)
              src = lsBit(bitBoard);
              boards_legal_moves[offset + src] = 1.0;
              bitBoard2 = board->movesFromSquares[src];
              if (board->fen->sideToMove == color) {
                if (pn < rook) {
                  attackedSuperior |= bitBoard2 & (oRooks | oQueens);
                  controlledByInferior |= bitBoard2 & board->oPawnMoves;
                } else {
                  attackedSuperior |= bitBoard2 & oQueens;
                  controlledByInferior |= bitBoard2 & (board->oPawnMoves | board->oKnightMoves | board->oBishopMoves);
                }
                attackedUndefended |= bitBoard2 & oUndefended;
              } else {
                if (pn < rook) attackedByInferior |= bitBoard2 & (rooks | queens);
                else attackedByInferior |= bitBoard2 & queens;
              }
              while (bitBoard2) {
                dst = lsBit(bitBoard2);
                boards_legal_moves[offset2 + dst] = 1.0; //(board->fen->sideToMove == color) ? moveValue(board, src, dst) : 1.0;
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
      src = lsBit(bitBoard);
      boards_legal_moves[offset + src] = 1.0;
      bitBoard2 = board->movesFromSquares[src];
      if (board->fen->sideToMove == color) {
        attackedUndefended |= bitBoard2 & oUndefended;
        controlledByInferior |= bitBoard2 & (board->oPawnMoves | board->oKnightMoves | board->oBishopMoves | board->oRookMoves);
      }
      while (bitBoard2) {
        dst = lsBit(bitBoard2);
        boards_legal_moves[offset2 + dst] = 1.0; //board->fen->sideToMove == color ? moveValue(board, src, dst) : 1.0;
        bitBoard2 &= bitBoard2 - 1;    
      }
      bitBoard &= bitBoard - 1;
      /*if (bitBoard) {
        fprintf(stderr, "Warning: Extra %s detected, merging into queen channel\n", pieceName[shiftedColor | Queen]);
      }*/
    }
    channel += 2;
    
    //one channel for white or black king occupation, plus another one for its moves or control squares
    bitBoard = board->occupations[shiftedColor | King];
    offset = sampleXchannels + channel * 64;
    offset2 = offset + 64;
    channel += 2;
    src = lsBit(bitBoard);
    boards_legal_moves[offset + src] = 1.0;
    bitBoard2 = board->movesFromSquares[src];
    if (board->fen->sideToMove == color) attackedUndefended |= bitBoard2 & oUndefended;
    while (bitBoard2) {
      dst = lsBit(bitBoard2);
      boards_legal_moves[offset2 + dst] = 1.0; //board->fen->sideToMove == color ? moveValue(board, src, dst) : 1.0;
      bitBoard2 &= bitBoard2 - 1;    
    }
  }

  // channel 65: attacked superior opponent's pieces defended or not
  if (attackedSuperior) offset = sampleXchannels + channel * 64;
  while (attackedSuperior) {
    src = lsBit(attackedSuperior);
    boards_legal_moves[offset + src] = 1.0;
    attackedSuperior &= attackedSuperior - 1;
  }
  channel++;

  // channel 66: attacked undefended opponent's pieces
  if (attackedUndefended) offset = sampleXchannels + channel * 64;
  while (attackedUndefended) {
    src = lsBit(attackedUndefended);
    boards_legal_moves[offset + src] = 1.0;
    attackedUndefended &= attackedUndefended - 1;
  }
  channel++;
  
  //channel 67: pieces attacked by opponent's inferior pieces
  if (attackedByInferior) offset = sampleXchannels + channel * 64;
  while (attackedByInferior) {
    src = lsBit(attackedByInferior);
    boards_legal_moves[offset + src] = 1.0;
    attackedByInferior &= attackedByInferior - 1;
  }
  channel++;
  
  //channel 68: dst squares controlled by opponent's inferior pieces
  if (controlledByInferior) offset = sampleXchannels + channel * 64;
  while (controlledByInferior) {
    src = lsBit(controlledByInferior);
    boards_legal_moves[offset + src] = 1.0;
    controlledByInferior &= controlledByInferior - 1;
  }
  channel++;    
  
  //legal moves for masking model predictions: channels 69 to 131 (total 132 channels)
  //but only the first 68 are used for training (68 channels x 64 squares = 4352 input features)
  //model output for moves is 4096 (64 src squares x 64 dst squares)
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
