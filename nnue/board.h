#ifndef BOARD_INCLUDED
#define BOARD_INCLUDED
//namespace Creatica {
  struct Board {
      //these booleans are not strictly needed except maybe isChess960
      bool isCheck = false;
      bool isStaleMate = false;
      bool isMate = false;
      bool isChess960 = false;
      unsigned char sideToMove = 0;
      unsigned char enPassant = 8;
      unsigned char halfmoveClock = 0;
      unsigned char num_moves = 0; //not necessary but good for 8-byte alignment on 64-bit systems
      //8 bytes up to here
      int moveNumber = 1; //may be unsigned short but will be padded 2 bytes anyway
      unsigned char castlingRook[2][2] = {{0, 7}, {0, 7}};
      //16 bytes up to here
      //the rest is aligned on 8-byte boundary
      unsigned char piecesOnSquares[64] = {7}; //64 bytes
      //unsigned long long occupations[2][7] = {0}; //[color][pieceType] 14 8-byte bitboards
      //alternative compact representation has only 8 8-byte bitboards - much better!
      //for example, white knights = side[ColorWhite] & pieceTypes[Knight] - trade off between memory and cpu
      unsigned long long side[2] = {0}; //all white and all black
      unsigned long long pieceTypes[6] = {0}; //all pawns, knights, bishops, rooks, queens and kings
      //would be just 16 + 64 + 64 = 144 bytes instead of 192
      //192 bytes total
  };
//}
#endif
