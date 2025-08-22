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
// use five different channels:
// 1. attacked superior opponent's pieces defended or not
// 2. attacked undefended opponent's pieces
// 3. pieces attacked by opponent's inferior pieces
// 4. dst squares controlled by opponent's inferior pieces
// 5. dst undefended squares controlled by opponent's pieces
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
  //unsigned long knights = board->occupations[sideToMove | Knight], 
                //bishops = board->occupations[sideToMove | Bishop], 
                //rooks = board->occupations[sideToMove | Rook], 
                //queens = board->occupations[sideToMove | Queen], 
                //all = board->occupations[sideToMove | PieceTypeAny],
                //allMoves = board->pawnMoves | board->knightMoves | board->bishopMoves | board->rookMoves | board->queenMoves | board->kingMoves,
                //oKnights = board->occupations[oppositeSide | Knight],
                //oBishops = board->occupations[oppositeSide | Bishop],
                //oRooks = board->occupations[oppositeSide | Rook],
                //oQueens = board->occupations[oppositeSide | Queen],
                //oAll = board->occupations[oppositeSide | PieceTypeAny], 
                //oAllMoves = board->oPawnMoves | board->oKnightMoves | board->oBishopMoves | board->oRookMoves | board->QueenMoves | board->oKingMoves,
                //oUndefended = (oAll ^ board->defendedPieces) & oAll, 
                //attackedSuperior = 0, 
                //attackedUndefended = 0, 
                //attackedByInferior = 0, 
                //controlledByInferior = 0;
  //unsigned char pnCount[16][64] = {{0}, {0}}; //how many piece names attack/defend a square

  for (int c = 0; c < 2; c++) {
    enum Color color = c == 0 ? board->opponentColor : board->fen->sideToMove;
    unsigned char shiftedColor = color << 3;
    //8 channels for white or black pawns and 8 channels for their moves or control squares (diag moves)
    //opponent's channels are first, sideToMove channels are second
    enum PieceName pawn = shiftedColor | Pawn;
    bitBoard = board->occupations[pawn];
    for (enum Files f = FileA; f <= FileH; f++) {
      if (bitBoard) {
        offset = sampleXchannels + channel * 64; 
        offset2 = offset + 64 * 42; //move control squares and moves channels to the end
        src = lsBit(bitBoard); //src square for a pawn
        boards_legal_moves[offset + src] = 1.0;
        //printf("boardLegalMoves(%d): %s board->channel[%s] = %d\n", omp_get_thread_num(), pieceName[pawn], squareName[src], channel - 21);
        board->channel[src] = channel - 21; // 0 to 7 - pawn channels for moves (channel is incremented from 0 to 41)
        board->sourceSquare[channel - 21] = src;
        bitBoard2 = board->movesFromSquares[src]; //all moves or control squares from src square
/*
        if (board->fen->sideToMove == color) {
          attackedSuperior |= bitBoard2 & (oKnights | oBishops | oRooks | oQueens);
          attackedUndefended |= bitBoard2 & oUndefended;
        } else {
          attackedByInferior |= bitBoard2 & (knights | bishops | rooks | queens);
        }
*/
        while (bitBoard2) { //loop over pawn moves (for opponent these are just diagonal pawn moves)
          dst = lsBit(bitBoard2);
          /*
          if (board->fen->sideToMove == color) {
            unsigned char diff = src > dst ? src - dst : dst - src;
            if (diff == 7 || diff == 9) pnCount[pawn][dst]++; //just diag moves for sideToMove pawns
          } else pnCount[pawn][dst]++;*/
          boards_legal_moves[offset2 + dst] = 1.0; 
          bitBoard2 &= bitBoard2 - 1;    
        }
        bitBoard &= bitBoard - 1;
      } //end of if (bitBoard)
      channel++;
    } //end of for loop over chessboard files from A to H
    //12 channels for pairs of white or black knights, bishops, rooks and queens (3 channels per type to handle promotions)
    //plus another 12 channels for either their moves or control squares
    //if there are more than a pair (very rare), we add extra piece(s) to the same channel(s)
    enum PieceName knight = shiftedColor | Knight;
    enum PieceName rook = shiftedColor | Rook;
    enum PieceName queen = shiftedColor | Queen;
    for (enum PieceName pn = knight; pn <= queen; pn++) {
      bitBoard = board->occupations[pn];
      for (int i = 0; i < 3; i++) { //three channels for knights, bishops, rooks and queens to handle promotions
        if (bitBoard) {
          offset = sampleXchannels + channel * 64;
          offset2 = offset + 64 * 42;
          src = lsBit(bitBoard);
          //printf("boardLegalMoves(%d): %s board->channel[%s] = %d\n", omp_get_thread_num(), pieceName[pn], squareName[src], channel - 21);
          board->channel[src] = channel - 21; // 8-10 (knight move channels), 11-13 (bishop), 14-16 (rook), 17-19 (queen)
          board->sourceSquare[channel - 21] = src;
          boards_legal_moves[offset + src] = 1.0;
          bitBoard2 = board->movesFromSquares[src];
/*
          if (board->fen->sideToMove == color) {
            if (pn < rook) {
              attackedSuperior |= bitBoard2 & (oRooks | oQueens);
              controlledByInferior |= bitBoard2 & board->oPawnMoves;
            } else if (pn < queen) {
              attackedSuperior |= bitBoard2 & oQueens;
              controlledByInferior |= bitBoard2 & (board->oPawnMoves | board->oKnightMoves | board->oBishopMoves);              
            } else {
              attackedUndefended |= bitBoard2 & oUndefended;
              controlledByInferior |= bitBoard2 & (board->oPawnMoves | board->oKnightMoves | board->oBishopMoves | board->oRookMoves);
            }
            attackedUndefended |= bitBoard2 & oUndefended;
          } else {
            if (pn < rook) attackedByInferior |= bitBoard2 & (rooks | queens);
            else attackedByInferior |= bitBoard2 & queens;
          }
*/
          while (bitBoard2) {
            dst = lsBit(bitBoard2);
            //pnCount[pn][dst]++;
            boards_legal_moves[offset2 + dst] = 1.0; 
            bitBoard2 &= bitBoard2 - 1;    
          }
          bitBoard &= bitBoard - 1;
          if (bitBoard && i == 2) {
            fprintf(stderr, "boardLegalMoves(%d) warning: 4th %s detected, skiping this unusual rare position to maintain 21 channels...\n", omp_get_thread_num(), pieceName[pn]);
            memset(boards_legal_moves + sampleXchannels * sizeof(float), 0, sizeof(float) * channels * 64);
            return 1;
          }
        }
        channel++;
      }
    }
    
    //one channel for white or black king occupation, plus another one for its moves or control squares
    enum PieceName king = shiftedColor | King;
    bitBoard = board->occupations[king];
    offset = sampleXchannels + channel * 64;
    offset2 = offset + 64 * 42;
    src = lsBit(bitBoard);
    //printf("boardLegalMoves(%d): %s board->channel[%s] = %d\n", omp_get_thread_num(), pieceName[king], squareName[src], channel - 21);
    board->channel[src] = channel - 21; // channel 20 is king moves
    board->sourceSquare[channel - 21] = src;
    boards_legal_moves[offset + src] = 1.0;
    bitBoard2 = board->movesFromSquares[src];
    //if (board->fen->sideToMove == color) attackedUndefended |= bitBoard2 & oUndefended;
    while (bitBoard2) {
      dst = lsBit(bitBoard2);
      //pnCount[king][dst]++;
      boards_legal_moves[offset2 + dst] = 1.0;
      bitBoard2 &= bitBoard2 - 1;    
    }
    channel++;
  }
  channel += 42; //42 channels for moves and control squares
  
/*
  // channel 42: attacked superior opponent's pieces defended or not
  //this does not work
  if (attackedSuperior) offset = sampleXchannels + channel * 64;
  while (attackedSuperior) {
    dst = lsBit(attackedSuperior);
    boards_legal_moves[offset + dst] = 1.0; //take a superior piece from this square
    attackedSuperior &= attackedSuperior - 1;
  }
  channel++;
  // channel 43: attacked undefended opponent's pieces
  //this does not work
  if (attackedUndefended) offset = sampleXchannels + channel * 64;
  while (attackedUndefended) {
    dst = lsBit(attackedUndefended);
    boards_legal_moves[offset + dst] = 1.0; //take a piece from this undefended square
    attackedUndefended &= attackedUndefended - 1;
  }
  channel++;
  
  //channel 44: pieces attacked by opponent's inferior pieces
  //this does not work
  if (attackedByInferior) offset = sampleXchannels + channel * 64;
  while (attackedByInferior) {
    src = lsBit(attackedByInferior);
    boards_legal_moves[offset + src] = 1.0; //move away from this square attacked by inferior piece
    attackedByInferior &= attackedByInferior - 1;
  }
  channel++;
  
  //channel 45: dst squares controlled by opponent's inferior pieces
  //it's not perfect as sideToMove even more inferior pieces could still move to these squares!
  //so this should be multiple per piece channels!
  if (controlledByInferior) offset = sampleXchannels + channel * 64;
  while (controlledByInferior) {
    dst = lsBit(controlledByInferior);
    boards_legal_moves[offset + dst] = 1.0; //avoid moves to these controlled by inferior pieces squares
    controlledByInferior &= controlledByInferior - 1;
  }
  channel++;
  
  //channel 46: dst undefended squares controlled by opponent's pieces - avoid moves to these squares
  //without actually making a move, it is impossible to figure out for sure if dst square is undefended or not!

  offset = sampleXchannels + channel * 64;
  channel++;
  //channels 42 to 62 are for opponent's control squares (21 channels)
  //channels 63 to 83 are for sideToMove moves (21 channels)
  //84 total channels (84 channels x 64 squares = 5376 input features)
  //model output should be also 16 channels flattened to 1024 but could be problematic because of promotions
  //in theory up to 8 channels could be reserved for promotions, 
  //so total 24 channels are still better than 64 for source squares! 
  //Reusing free pawns' channels might be confusing. 
  //In practise we could have three channels for knights, bishops, rooks and queens to handle promotions, 
  //i.e. extra 5 channels per color. 
  //So the model move prediction branch has 21 channels x 64 dst squares = 1344 output logits.
  enum PieceName pawn = sideToMove | Pawn;
  enum PieceName oPawn = oppositeSide | Pawn;
  enum PieceName knight = sideToMove | Knight;
  enum PieceName oKnight = oppositeSide | Knight;
  enum PieceName bishop = sideToMove | Bishop;
  enum PieceName oBishop = oppositeSide | Bishop;
  enum PieceName rook = sideToMove | Rook;
  enum PieceName oRook = oppositeSide | Rook;
  enum PieceName queen = sideToMove | Queen;
  enum PieceName oQueen = oppositeSide | Queen;
  enum PieceName king = sideToMove | King;
  enum PieceName oKing = oppositeSide | King;
  bitBoard = all; //all sideToMove occupations
  //for (src = SquareA1; src <= SquareH8; src++) { //not sure which loop is faster, probably depends on the number of pieces
  while (bitBoard) { //loop over all sideToMove pieces
    src = lsBit(bitBoard); //sideToMove piece src square
    bitBoard2 = board->sideToMoveMoves[src]; //all moves from src square
    while (bitBoard2) { //loop over moves from src square
      dst = lsBit(bitBoard2); // destination square for a move
      float value = pieceValue[board->piecesOnSquares[dst] & 7] - pieceValue[board->piecesOnSquares[src] & 7];
      enum PieceName last = board->piecesOnSquares[src];
      while (true) {
        if (pnCount[oPawn][dst]) {
          pnCount[oPawn][dst]--;
          value += pieceValue[Pawn];
          last = oPawn;
        } else if (pnCount[oKnight][dst]) {
          pnCount[oKnight][dst]--;
          value += pieceValue[Knight];
          last = oKnight;
        } else if (pnCount[oBishop][dst]) {
          pnCount[oBishop][dst]--;
          value += pieceValue[Bishop];
          last = oBishop;
        } else if (pnCount[oRook][dst]) {
          pnCount[oRook][dst]--;
          value += pieceValue[Rook];
          last = oRook;
        } else if (pnCount[oQueen][dst]) {
          pnCount[oQueen][dst]--;
          value += pieceValue[Queen];
          last = oQueen;
        } else if (pnCount[oKing][dst]) {
          last = oKing;
        } else break;
        if (pnCount[pawn][dst]) {
          pnCount[pawn][dst]--;
          value -= pieceValue[Pawn];
          last = pawn;
        } else if (pnCount[knight][dst]) {
          pnCount[knight][dst]--;
          value -= pieceValue[Knight];
          last = knight;
        } else if (pnCount[bishop][dst]) {
          pnCount[bishop][dst]--;
          value -= pieceValue[Bishop];
          last = bishop;
        } else if (pnCount[rook][dst]) {
          pnCount[rook][dst]--;
          value -= pieceValue[Rook];
          last = rook;
        } else if (pnCount[queen][dst]) {
          pnCount[queen][dst]--;
          value -= pieceValue[Queen];
          last = queen;
        } else if (pnCount[king][dst]) {
          last = king;
        } else break;
      }
      enum PieceType pt = last & 7;
      if (pt != King) {
        if ((last >> 3) == board->fen->sideToMove)
          value += pieceValue[pt]; //last piece stays on board after all exchanges, so we need to keep its value
        else 
          value -= pieceValue[pt];        
      }
      printf("boardLegalMoves(%d): %s move from %s to %s value %f\n", omp_get_thread_num(), pieceName[board->piecesOnSquares[src]], squareName[src], squareName[dst], value);
      if (value < 0)
        boards_legal_moves[offset + dst] = 1.0; //avoid moves to undefended squares
      bitBoard2 &= bitBoard2 - 1;    
    } //end of while (bitBoard2)
    bitBoard &= bitBoard - 1;    
  } //end while (bitBoard)
*/
  //sanity check, could be commented out once verified
  if (channel != channels) {
	  printf("boardLegalMoves(%d) error: number of channels (%d) != channels (%d)\n", omp_get_thread_num(), channel, channels);
	  return -2;
	}
	
	return 0;
}
