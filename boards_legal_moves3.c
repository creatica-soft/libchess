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

//this function fills BMPR structure member - an array of boards_legal_moves
int boardLegalMoves(float * boards_legal_moves, int sample, int channels, struct Board * board) {
	unsigned char channel = 0;
  unsigned long bitBoard, bitBoard2;
	int offset;
	int sampleXchannels = sample * channels * 64;
	enum SquareName s, s2;
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
  /*
  //Channel 0 is all pieces for the sideToMove
  offset = sampleXchannels;// + channel * 64;
  int shiftedColor = board->fen->sideToMove << 3;
  enum PieceName pnStart = shiftedColor | Pawn;
  enum PieceName pnEnd = shiftedColor | King;
  for (enum PieceName pn = pnStart; pn <= pnEnd; pn++) { //sideToMove pieces
    bitBoard = board->occupations[pn];
    while (bitBoard) {
      s = __builtin_ctzl(bitBoard);
      boards_legal_moves[offset + s] = pieceValue[board->piecesOnSquares[s] & 7];
      bitBoard &= bitBoard - 1;
    }
  }
  //channel++;
  
  //Channel 1 is for all pieces of the opponent
  offset = sampleXchannels + 64; // * channel;
  shiftedColor = board->opponentColor << 3;
  pnStart = shiftedColor | Pawn;
  pnEnd = shiftedColor | King;
  for (enum PieceName pn = pnStart; pn <= pnEnd; pn++) { //opponent pieces
    bitBoard = board->occupations[pn];
    while (bitBoard) {
      s = __builtin_ctzl(bitBoard);
      boards_legal_moves[offset + s] = pieceValue[board->piecesOnSquares[s] & 7];
      bitBoard &= bitBoard - 1;
    }
  }
  */
  /*
  //channels 2 to 65 (64 channels) for opponent's controled squares
  channel = 2;
  for (enum SquareName sq = SquareA1; sq <= SquareH8; sq++) {
    if (board->piecesOnSquares[sq] == PieceNameNone || (board->piecesOnSquares[sq] >> 3) != board->opponentColor) {
      channel++;     
      continue;
    }
    offset = sampleXchannels + channel * 64;
    bitBoard = board->movesFromSquares[sq];
    while (bitBoard) {
      s = __builtin_ctzl(bitBoard);
      boards_legal_moves[offset + s] = pieceValue[board->piecesOnSquares[sq] & 7];
      bitBoard &= bitBoard - 1;    
    }
    channel++;
  }
	*/
  
	//white occupation bitboards - 6 channels from 0 (pawns) to 5 (king)
	//maybe, a simpler input could work as well such as all white pieces with their values for one channel
	//and all black pieces with their values for another - no, the model struggle to learn!
	//And maybe alternating these channels based on sideToMove is a good idea?
  //enum PieceName pnStart = (board->fen->sideToMove << 3) | Pawn;
  //enum PieceName pnEnd = (board->fen->sideToMove << 3) | King;
  for (enum PieceName pn = WhitePawn; pn <= WhiteKing; pn++) { //white pieces
    //printf("pieceName %s occupation %lu\n", pieceName[pn], board->occupations[pn]);
    offset = sampleXchannels + channel * 64;
    bitBoard = board->occupations[pn];
    while (bitBoard) {
      s = __builtin_ctzl(bitBoard);
      boards_legal_moves[offset + s] = 1.0;
      bitBoard &= bitBoard - 1;
    }
    channel++;
  }
  
	//black occupation bitboards - 6 channels from 6 (pawns) to 11 (king) 
	//as well as squares controlled by opponent pieces - 16 channels from 12 to 27
	//pawn (0.1), knight (0.3), bishop (0.32), rook (0.5), queen (0.9), king (1.0)
  //enum PieceName pnoStart = (board->opponentColor << 3) | Pawn;
  //enum PieceName pnoEnd = (board->opponentColor << 3) | King;
  for (enum PieceName pn = BlackPawn; pn <= BlackKing; pn++) { //black pieces
    //printf("pieceName %s occupation %lu\n", pieceName[pn], board->occupations[pn]);
    offset = sampleXchannels + channel * 64;
    bitBoard = board->occupations[pn];
    while (bitBoard) {
      s = __builtin_ctzl(bitBoard);
      boards_legal_moves[offset + s] = 1.0;
      bitBoard &= bitBoard - 1;      
    }
    channel++;
  }
  
  //all white piece values (for material balance) - channel 12
  //printf("pieceName %s occupation %lu\n", pieceName[PieceNameWhite], board->occupations[PieceNameWhite]);
  offset = sampleXchannels + channel * 64;
  bitBoard = board->occupations[PieceNameWhite];
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = pieceValue[board->piecesOnSquares[s] & 7];
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  
  //all black piece values (for material balance) - channel 13
  //printf("pieceName %s occupation %lu\n", pieceName[PieceNameBlack], board->occupations[PieceNameBlack]);
  offset = sampleXchannels + channel * 64;
  bitBoard = board->occupations[PieceNameBlack];
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = pieceValue[board->piecesOnSquares[s] & 7];
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  /*
  //opponent pieces that it defends - channel 14
  //printf("defended opponent's pieces %lu\n", board->defendedPieces);    
  offset = sampleXchannels + channel * 64;
  bitBoard = board->defendedPieces;
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //sideToMove pieces that opponent attacks  - channel 15
  //printf("attacked sideToMove pieces %lu\n", board->attackedPieces);    
  offset = sampleXchannels + channel * 64;
  bitBoard = board->attackedPieces;
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //channels 14 and 15 (opponent's defended pieces and sideToMove attacked pieces) seem to create
  //a disbalance. What about sideToMove defeneded pieces and opponent's pieces that sideToMove attacks?
  //are these channels even important?
  
  //sideToMove pieces that it defends - channel 16
  //printf("defended sideToMove pieces %lu\n", ...);    
  offset = sampleXchannels + channel * 64;
  bitBoard = board->attackedSquares & board->occupations[(board->fen->sideToMove << 3) | PieceTypeAny];
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //opponent's pieces that sideToMove attacks  - channel 17
  //printf("attacked opponent's pieces %lu\n", ...);    
  offset = sampleXchannels + channel * 64;
  bitBoard = board->attackedSquares & board->occupations[(board->opponentColor << 3) | PieceTypeAny];
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  
  //if king is checked, then the squares that could be used to block the check - channel 18
  //printf("blockingSquares %lu\n", board->blockingSquares);    
  offset = sampleXchannels + channel * 64;
  bitBoard = board->blockingSquares;
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;        
  }
  channel++;
  
  //pieces that check king - channel 19
  //printf("checkers %lu\n", board->checkers);    
  offset = sampleXchannels + channel * 64;
  bitBoard = board->checkers;
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //pieces that are pinned - channel 20
  //printf("pinnedPieces %lu\n", board->pinnedPieces);    
  offset = sampleXchannels + channel * 64;
  bitBoard = board->pinnedPieces;
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //pieces that pin - channel 21
  //printf("pinningPieces %lu\n", board->pinningPieces);    
  offset = sampleXchannels + channel * 64;
  bitBoard = board->pinningPieces;
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //en passant bit (legal) - channel 22
  //not needed as legal moves are provided
  //printf("enPassantLegalBit %lu\n", board->fen->enPassantLegalBit);    
  offset = sampleXchannels + channel * 64;
  bitBoard = board->fen->enPassantLegalBit;
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //castling rights - channel 23 - probably not needed because they included in legal moves
  //printf("castlingBits %lu\n", board->fen->castlingBits);   
  offset = sampleXchannels + channel * 64;
  bitBoard = board->fen->castlingBits;
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = 1.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
  
  //square advantage from the side to move perspective - non-binary channel 22
  //this is wrong, it does not factor in that the last piece in exchange will stay on board
  //perhaps, it is better to provide non-binary 64 maps of pieces that attack/defend each of 64 squares.
  //Opposite to legal moves. Legal moves provide binary 64 maps of destinations for each square
  //square advantage would provide a non-binary maps for pieces that can move to or control the square
  //piece values would be negative for the side to move and positive for the opponent
  //or maybe a simpler approach of binary channel where 1 encourages the exchange on a square and 0 
  //doesn't? Again, it won't be perfect because of possible pins of the opponent's pieces, which can't 
  //really participate in exchange
  //offset = sampleXchannels + channel * 64;
  //for (enum SquareName sq = SquareA1; sq <= SquareH8; sq++)
  //  boards_legal_moves[offset + sq] = board->squareCostForOpponent[sq] - board->squareCostForSideToMove[sq];
  //channel++;

  //promo distance - channel 4 //channel 24
  //maybe not needed in a simple model
  offset = sampleXchannels + channel * 64;
  bitBoard = board->occupations[WhitePawn];
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = (7.0 - (float)(s >> 3)) / 6.0;
    bitBoard &= bitBoard - 1;    
  }
  bitBoard = board->occupations[BlackPawn];
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    boards_legal_moves[offset + s] = (float)(s >> 3) / 6.0;
    bitBoard &= bitBoard - 1;    
  }
  channel++;
 
  //mobility of sideToMove pieces scaled by their values and the pieces that can move - 2 channels 25 - 26
  //in a simple model not needed, legal moves could probably compensate it
  offset = sampleXchannels + channel * 64;
  channel++;
  offset2 = sampleXchannels + channel * 64;
  bitBoard = board->occupations[(board->fen->sideToMove << 3) | PieceTypeAny];
  while (bitBoard) {
    s = __builtin_ctzl(bitBoard);
    unsigned long bitBoardMoves = board->sideToMoveMoves[s];
    enum PieceType piece = board->piecesOnSquares[s] & 7;
    boards_legal_moves[offset + s] = (float)__builtin_popcountl(bitBoardMoves) / pieceMobility[piece] * pieceValue[piece];
    if (bitBoardMoves > 0) boards_legal_moves[offset2 + s] = 1.0;
    bitBoard &= bitBoard - 1;
  }
  channel++;
  */
  
  //square advantage for each square - channels from 27 to 90 (64 channels)
  //probably just confuse the model
  /*
  for (enum SquareName sq = SquareA1; sq <= SquareH8; sq++) {
    offset = sampleXchannels + channel * 64;
    unsigned long bitSquare = (1UL << sq);
	  //printf("square advantage for square %s %f\n", squareName[sq], ...);    
    for (enum SquareName sqr = SquareA1; sqr <= SquareH8; sqr++) {
      if (board->movesFromSquares[sqr] & bitSquare) {
        float value = pieceValue[board->piecesOnSquares[sqr] & 7];
        boards_legal_moves[offset + sqr] = (board->piecesOnSquares[sqr] >> 3) == board->fen->sideToMove ? -value : value;
      }
    }
    //we include piece value on the square as well if any
    float value = pieceValue[board->piecesOnSquares[sq] & 7];
    boards_legal_moves[offset + sq] = (board->piecesOnSquares[sq] >> 3) == board->fen->sideToMove ? -value : value;
    channel++;
  }
  */

  //legal moves from each square - 64 channels 14 to 77 (total 78 channels)
  //perhaps, in a simple model, we could use not just legal moves but also opponent's control squares
  //in separate channels and instead of binary channels, use piece values for their moves?
  //channel = 14;
  for (enum SquareName sq = SquareA1; sq <= SquareH8; sq++) {
    offset = sampleXchannels + channel * 64;
	  //printf("legal moves from square %s %lu\n", squareName[sq], board->sideToMoveMoves[sq]);    
    bitBoard = board->sideToMoveMoves[sq];
    while (bitBoard) {
      s = __builtin_ctzl(bitBoard);
      enum PieceType pt = board->piecesOnSquares[sq] & 7;
      bool moveSet = false;
      int shiftedColor = board->opponentColor << 3;
      if (pt < King) {
        for (enum PieceType p = Queen; p > pt; p--) {
          bitBoard2 = board->occupations[shiftedColor | p];
          while (bitBoard2) { //for all opponent pieces that greater than pt
            s2 = __builtin_ctzl(bitBoard2);
            if (s == s2) {
              boards_legal_moves[offset + s] = 1.0; //prioritise capture of major pieces
              moveSet = true;
              break;
            }
            bitBoard2 &= bitBoard2 - 1;
          }
          if (moveSet) break;           
        }
        for (enum PieceType p = pt; p >= Pawn; p--) { 
          bitBoard2 = board->occupations[shiftedColor | p];
          while (bitBoard2) { //for all opponent pieces that greater than pt
            s2 = __builtin_ctzl(bitBoard2);
            if (s == s2 && !(board->defendedPieces & s2)) {
              boards_legal_moves[offset + s] = 1.0; //prioritise capture of underfended pieces equal or lower than pt
              moveSet = true;
              break;
            }
            bitBoard2 &= bitBoard2 - 1;
          }
          if (moveSet) break;        
        }
        for (enum PieceType p = Pawn; p < pt; p++) { 
          bitBoard2 = board->occupations[shiftedColor | p];
          while (bitBoard2) { //for all opponent pieces that smaller than pt
            s2 = __builtin_ctzl(bitBoard2);
            if ((board->movesFromSquares[s2] & sq) && !(board->movesFromSquares[s2] & s)) {
              boards_legal_moves[offset + s] = 1.0; //prioritise moves for pieces attacked by a minor piece
              moveSet = true;
              break;
            }
            if (board->movesFromSquares[s2] & s) {
              boards_legal_moves[offset + s] = 0.001; //avoid moves to squares attacked by a minor piece
              moveSet = true;
              break;
            }
            bitBoard2 &= bitBoard2 - 1;
          }
          if (moveSet) break;
        }
      }
      if (!moveSet) boards_legal_moves[offset + s] = 0.5; //pieceValue[board->piecesOnSquares[sq] & 7];
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
