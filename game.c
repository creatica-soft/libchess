#pragma warning(disable:4996)
#define __STDC_WANT_LIB_EXT1__ 1
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <pthread.h> //use -pthread when compiling and linking
#include <sqlite3.h>
#include "uthash.h"
#include "libchess.h"

sqlite3 * openDb(const char *, int);
void closeDb(sqlite3 *);
int gameExists(sqlite3 *, sqlite3_int64);
int insertGame(sqlite3 *, sqlite3_int64, char *);
void updateNextMove(sqlite3 *, sqlite3_int64, const char *, int);
int getNextMoves(sqlite3 *, sqlite3_int64, struct MoveScoreGames **);
struct GameHash {
    uint8_t hash[8];    // Key: 8-byte half-MD5 hash used to filter identical games
    UT_hash_handle hh;  // Required for uthash
};
extern struct GameHash * games;

static unsigned long gameNumber, numberOfGames, fileNumber;
static unsigned long gameStartPositions[MAX_NUMBER_OF_GAMES];
static pthread_mutex_t gameNumber_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fileNumber_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t queueGames_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ecoLines_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t queueMoves0_mutex;
static pthread_mutex_t queueMoves1_mutex;
static pthread_mutex_t queueMoves2_mutex;
static pthread_mutex_t queueMoves3_mutex;
static pthread_mutex_t queueMoves4_mutex;
static pthread_mutex_t queueMoves5_mutex;
static pthread_mutex_t queueMoves6_mutex;
static pthread_mutex_t queueMoves7_mutex;
struct GenEndGamesCtx {
  int threadId;
  int maxGameNumber;
  int maxPieceNumber;
  char dataset[255];
  char engine[255];
  long movetime;
  int depth;
  int hashSize;
  int threadNumber;
  char syzygyPath[255];
  int multiPV;
  bool writedebug;
  int * genEndGamesResult;
};
struct InitGameFromPGNfileContext {
  bool generateZobristHash;
	char fileName[255];
	int threadNumber;
	int minElo;
	int maxEloDiff;
	int minMoves;
	int numberOfGames;
	int numberOfEcoLines;
	struct EcoLine ** ecoLines;	
	bool updateDb;
	bool createDataset;
	char dataset[255];
	sqlite3 * db;
	int sqlThreads;
	int * initGameResult;
};
struct InitGameFromPGNfilesContext {
  bool generateZobristHash;
	char fileNames[255][255];
	int numberOfFiles;
	int threadNumber;
	int minElo;
	int maxEloDiff;
	int minMoves;
	int numberOfGames;
	int numberOfEcoLines;
	struct EcoLine ** ecoLines;
	bool updateDb;
	bool createDataset;
	char dataset[255];
	sqlite3 * db;
	int sqlThreads;
	int * initGameResult;
};
struct NodeMoves {
  unsigned long hash;
  char score;
  char nextMove[6];
  struct NodeMoves * next;
};
struct QueueMoves {
  struct NodeMoves * first;
  struct NodeMoves * last;
  int len;
};
struct SqlContext {
  int threadNumber;
  struct QueueMoves * queue;
  pthread_mutex_t * mutex;
};
struct NodeGames {
  unsigned long hash;
  struct NodeGames * next;
};
struct QueueGames {
  struct NodeGames * first;
  struct NodeGames * last;
  int len;
};

static struct QueueMoves * nextMoves0 = NULL;
static struct QueueMoves * nextMoves1 = NULL;
static struct QueueMoves * nextMoves2 = NULL;
static struct QueueMoves * nextMoves3 = NULL;
static struct QueueMoves * nextMoves4 = NULL;
static struct QueueMoves * nextMoves5 = NULL;
static struct QueueMoves * nextMoves6 = NULL;
static struct QueueMoves * nextMoves7 = NULL;

void enqueueMoves(struct QueueMoves * queue, unsigned long hash, char * nextMove, char score) {
  if (queue == NULL) return;
  struct NodeMoves * node = (struct NodeMoves *)malloc(sizeof(struct NodeMoves));
  node->hash = hash;
  node->score = score;
  strncpy(node->nextMove, nextMove, 6);
  node->next = NULL;
  if (queue->last == NULL)
    queue->first = queue->last = node;
  else { 
    queue->last->next = node;
    queue->last = node;
  }
  queue->len++;
}
void dequeueMoves(struct QueueMoves * queue) {
  if (queue == NULL) return;
  if (queue->first == NULL) return;
  struct NodeMoves * node = queue->first;
  queue->first = queue->first->next;
  queue->len--;
  if (queue->len == 0) queue->last = NULL;
  free(node);
}
bool isEmptyLine(char * line) {
	size_t len = strlen(line);
	for (char c = 0; c < len; c++) {
		if (!isspace(line[c])) return false;
	}
	return true;
}

void stripGameResult(struct Game * game) {
	char * endOfMoves = strstr(game->sanMoves, "1-0");
	if (!endOfMoves) {
		endOfMoves = strstr(game->sanMoves, "0-1");
		if (!endOfMoves) {
			endOfMoves = strstr(game->sanMoves, "1/2-1/2");
			if (!endOfMoves) {
				endOfMoves = strstr(game->sanMoves, "*");
				if (endOfMoves) {
      	  if (strcmp(game->tags[Result], "*") != 0) {
    	      printf("stripGameResult() warning: PGN tag Result \'%s\' does not match the end of moves result \'*\' - updated the tag!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
  	        strcpy(game->tags[Result], "*");
  	      }
  	    } else {
  	      printf("stripGameResult() warning: missing the end of moves result!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n[Result \"%s\"]\n", game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black], game->tags[Result]);
  	    }
			} else {
    	  if (strcmp(game->tags[Result], "1/2-1/2") != 0) {
  	      printf("stripGameResult() warning: PGN tag Result \'%s\' does not match the end of moves result \'1/2-1/2\' - updated the tag!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
  	      strcpy(game->tags[Result], "1/2-1/2");
  	    }
			}
		} else {
      if (strcmp(game->tags[Result], "0-1") != 0) {
  	    printf("stripGameResult() warning: PGN tag Result \'%s\' does not match the end of moves result \'0-1\' - updated the tag!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
  	    strcpy(game->tags[Result], "0-1");
  	  }
		}
	} else {
      if (strcmp(game->tags[Result], "1-0") != 0) {
  	    printf("stripGameResult() warning: PGN tag Result \'%s\' does not match the end of moves result \'1-0\' - updated the tag!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
    	  strcpy(game->tags[Result], "1-0");
    	}
	}
	if (endOfMoves) game->sanMoves[endOfMoves - game->sanMoves] = '\0';
}

//strips annotations, variations, etc 
//returns number of plies
int normalizeMoves(char * moves) {
	char excludeChars[] = { '\n', '\v', '\t', ' ', '!', '?', '\r', '\1', '\2', '\3', '\4', '\5', '\6', '\7', '\10', '\16', '\17', '\20', '\21', '\22', '\23', '\24', '\25', '\26', '\27', '\30', '\31', '\32', '\33', '\34', '\35', '\36', '\37' };
	bool moreThanOne = false, skipCur = false, skipPar = false, dollar = false;
	int count = 0;
	unsigned short n = 0, numberOfPlies = 0;
	char * s = strndup(moves, strlen(moves));
	if (!s) {
	  printf("normalizeMoves() error: strndup returned NULL: %s moves: %s\n", strerror(errno), moves);
	  return errno;
	}
	size_t len = strlen(s);
	for (int c = 0; c < len; c++) {
		if (skipCur) {
			if (s[c] == '}')
				skipCur = false;
			continue;
		}
		if (s[c] == ';') break;
		if (s[c] == '{') {
			skipCur = true;
			continue;
		}
		if (skipPar) {
			if (s[c] == ')') {
				count--;
				if (count == 0)
					skipPar = false;
			}
			else if (s[c] == '(')
				count++;
			continue;
		}
		if (s[c] == '(') {
			skipPar = true;
			count++;
			continue;
		}
		if (dollar) {
			if (isdigit(s[c])) continue;
			else dollar = false;
		}
		if (s[c] == '$') {
			dollar = true;
			continue;
		}
		if (memchr(excludeChars, s[c], sizeof excludeChars / sizeof excludeChars[0])) {
			if (!moreThanOne) {
				if (n > 0) {
					moves[n++] = ' ';
					numberOfPlies++;
					moreThanOne = true;
				}
			}
		} else {
			moreThanOne = false;
			moves[n++] = s[c];
		}
	}
	free(s);
	moves[n] = '\0';
	return numberOfPlies;
}

//strips move numbers
//returns number of plies
int movesOnly(char * moves) {
	bool wroteSpace = false;
	unsigned short n = 0, numberOfPlies = 0;;
	char * s = strndup(moves, strlen(moves));
	if (!s) {
	  printf("movesOnly() error: strndup() returned NULL: %s. moves %s\n", strerror(errno), moves);
	  return errno;
	}
	size_t len = strlen(s);
	for (int c = 1; c < len; c++) {
		if (s[c] == '.') continue;
		if (s[c] == ' ') {
			if (s[c - 1] == ' ') continue;
			else if (!wroteSpace) {
				if (n > 0) {
				  moves[n++] = s[c];
				  wroteSpace = true;
				  numberOfPlies++;
				}
			}
		} else {
			if (isdigit(s[c])) {
				if (s[c - 1] == ' ' || isdigit(s[c - 1])) continue;
				else {
					moves[n++] = s[c];
					if (wroteSpace) wroteSpace = false;
				}
			} else {
				moves[n++] = s[c];
				if (wroteSpace) wroteSpace = false;
			}
		}
	}
	free(s);
	if (moves[n - 1] == ' ')
	  moves[n - 1] = '\0';
	else moves[n] = '\0';
	return numberOfPlies; 
}

///<summary>
/// Count number of games from a file stream and index them by a game start position
///</summary>
unsigned long countGames(FILE * file, char * firstLine, unsigned long gameStartPositions[], unsigned long maxNumberOfGames) {
	unsigned long numberOfGames = 0;
	char line[8];
	int res;
	long pos = ftell(file);
	while (fgets(line, sizeof line, file)) {
		if (strstr(line, firstLine)) {
			if (numberOfGames < maxNumberOfGames)
				gameStartPositions[numberOfGames++] = pos;
			else numberOfGames++;
		}
		pos = ftell(file);
	}
	return numberOfGames;
}

///<summary>
/// reads PGN game tags for a first game pointed by a file stream
/// and fills the provided array of struct Tag
/// returns 0 on success, non-zero on error
///</summary>
int gTags(Tag gameTags, FILE * file) {
	char line[MAX_TAG_NAME_LEN + MAX_TAG_VALUE_LEN + 5];

	//skip empty lines and moves
	while (fgets(line, sizeof line, file)) {
		if (line[0] == '[') break;
	}
	if (feof(file)) {
		//printf("gameTags(): end of file\n");
		return 1;
	}

	memset(gameTags, 0, sizeof(char[MAX_NUMBER_OF_TAGS][MAX_TAG_VALUE_LEN]));

	strtotag(gameTags, line);
	
	//Read other tags
	while (fgets(line, sizeof line, file)) {
		if (line[0] == '[') strtotag(gameTags, line);
		else break;
	}
	return 0;
}

int eTags(EcoTag ecoTags, FILE * file) {
	char line[MAX_ECO_TAG_NAME_LEN + MAX_TAG_VALUE_LEN + 5];

	//skip empty lines and moves
	while (fgets(line, sizeof line, file)) {
		if (line[0] == '[') break;
	}
	if (feof(file)) {
		//printf("eTags() error: unexpected end of file\n");
		return 1;
	}

	memset(ecoTags, 0, sizeof(char[MAX_NUMBER_OF_ECO_TAGS][MAX_TAG_VALUE_LEN]));

	strtoecotag(ecoTags, line);

	//Read other tags
	while (fgets(line, sizeof line, file)) {
		if (line[0] == '[') strtoecotag(ecoTags, line);
		else break;
	}
	return 0;
}

///<summary>
/// Initialize an array of ecoLine structures ecoLines[MAX_NUMBER_OF_ECO_LINES]
/// from ecoFileName pgn file and return numberOfEcoLines
///</summary>
int initEcoLines(char * ecoFileName, struct EcoLine ** ecoLines) {
  unsigned long numberOfEcoLines = 0;
	FILE * ecoFile = fopen(ecoFileName, "r");
	if (!ecoFile) {
		printf("initEcoLines() warning: failed to open a ECO file %s, %s\n", ecoFileName, strerror(errno));
		return 1;
	}
	unsigned long ecoLinesStartPositions[MAX_NUMBER_OF_ECO_LINES];
	numberOfEcoLines = countGames(ecoFile, "[ECO ", ecoLinesStartPositions, MAX_NUMBER_OF_ECO_LINES);
	if (numberOfEcoLines > MAX_NUMBER_OF_ECO_LINES) {
		printf("initEcoLines() warning: number of lines in a eco file %s is %lu, which is greater than the maximum %d\n", ecoFileName, numberOfEcoLines, MAX_NUMBER_OF_ECO_LINES);
		numberOfEcoLines = MAX_NUMBER_OF_ECO_LINES;
	}
	rewind(ecoFile);

	char ecoLine[80];
	for (unsigned long i = 0; i < numberOfEcoLines; i++) {
		ecoLines[i] = malloc(sizeof(struct EcoLine));
		if (!ecoLines[i]) {
			printf("initEcoLines() error: malloc failure\n");
			return 2;
		}
		if (eTags(ecoLines[i]->tags, ecoFile)) {
			printf("initEcoLines(): end of eco file %s\n", ecoFileName);
			free(ecoLines[i]);
			numberOfEcoLines = i;
			break;
		}

		//skip empty lines and tag lines            
		while (fgets(ecoLine, sizeof ecoLine, ecoFile)) {
			if (!isEmptyLine(ecoLine) && ecoLine[0] != '[') break;
		}
		ecoLines[i]->sanMoves[0] = '\0';

		//Read eco line moves until first empty line
		strcat(ecoLines[i]->sanMoves, ecoLine);
		while (fgets(ecoLine, sizeof ecoLine, ecoFile)) {
			if (isEmptyLine(ecoLine)) break;
			strcat(ecoLines[i]->sanMoves, ecoLine);
		}
		//strip the game result;
		char * endOfLines = strchr(ecoLines[i]->sanMoves, '*');
		if (endOfLines) memset(endOfLines, '\0', 1);

		//strip comments, variations and NAGs
		normalizeMoves(ecoLines[i]->sanMoves);

		//strip move numbers
		movesOnly(ecoLines[i]->sanMoves);
	}
	//classify = true;
	fclose(ecoFile);
	return numberOfEcoLines;
}

void genEndGame(int pieceMaxNumber, struct Board * board, enum Color sideToMove, enum CastlingRightsEnum castlingRights, enum Files enPassant, unsigned int threadId) {
  enum PieceName kings[2] = {WhiteKing, BlackKing};
  enum PieceName pawns[2] = {WhitePawn, BlackPawn};
  enum PieceName knights[2] = {WhiteKnight, BlackKnight};
  enum PieceName bishops[2] = {WhiteBishop, BlackBishop};
  enum PieceName rooks[2] = {WhiteRook, BlackRook};
  enum PieceName queens[2] = {WhiteQueen, BlackQueen};
  enum PieceName minors[4] = {WhiteKnight, WhiteBishop, BlackKnight, BlackBishop};
  enum PieceName majors[4] = {WhiteRook, WhiteQueen, BlackRook, BlackQueen};
  enum PieceName whitePieces[5] = {WhitePawn, WhiteKnight, WhiteBishop, WhiteRook, WhiteQueen};
  enum PieceName blackPieces[5] = {BlackPawn, BlackKnight, BlackBishop, BlackRook, BlackQueen};  
  int gameType;
  int pieceNumber = randomNumber(1, pieceMaxNumber);
  //int pieceNumber = pieceMaxNumber;
  //printf("genEndGame(%u): pieceNumber %d\n", pieceNumber, threadId);
  
  switch(pieceNumber) {
  	case 1: //3 pieces
      gameType = randomNumber(0, 1); //pawn, major
  		break;
  	case 2: //4 pieces
      gameType = randomNumber(0, 6); //2pawns, major+pawn, minor+pawn, major+minor, 2majors, 2minors, anyGame
  		//gameType = randomNumber(0, 4); //exclude 2minors and anyGame
  		break;
  	case 3: //5 pieces
      gameType = randomNumber(0, 7); //3pawns, major+2pawns, minor+2pawns, major+minor+pawn, 2majors+pawn, 2minors+pawn, major+2minors, anyGame
  		break;
  	default:
  		printf("genEndGame() error: currently only 5 pieces are supported, recevied %d\n", pieceMaxNumber + 2);
  		return;
  }

  enum PieceName pn[32];
  pn[0] = WhiteKing;
  pn[1] = BlackKing;
  
  if (sideToMove != ColorWhite && sideToMove != ColorBlack)
    sideToMove = randomNumber(0, 1);    
  if (pieceNumber == 1) {
    switch (gameType) {
      case 0: //pawn games with 3 pieces
        pn[2] = pawns[randomNumber(0, 1)];
        break;
      case 1: //major games with 3 pieces
        pn[2] = majors[randomNumber(0, 3)];
        break;
     }
  }
  else if (pieceNumber == 2) {
    switch (gameType) {
      case 0: // 2 pawns games with 4 pieces
        pn[2] = pawns[randomNumber(0, 1)];
        pn[3] = pawns[randomNumber(0, 1)];
        break;
      case 1: //major+pawn games with 4 pieces
        pn[2] = majors[randomNumber(0, 3)];
        pn[3] = pawns[randomNumber(0, 1)];  
        break;
      case 2: //minor+pawn games with 4 pieces
        pn[2] = minors[randomNumber(0, 3)];  
        pn[3] = pawns[randomNumber(0, 1)]; 
        break;
      case 3: //major+minor games with 4 pieces
        pn[2] = majors[randomNumber(0, 3)];
        pn[3] = minors[randomNumber(0, 3)];
      	break;
      case 4: //2majors games with 4 pieces
        pn[2] = majors[randomNumber(0, 3)];
        pn[3] = majors[randomNumber(0, 3)];
      	break;
      case 5: //2minors games with 4 pieces
        pn[2] = minors[randomNumber(0, 3)];
        pn[3] = minors[randomNumber(0, 3)]; 
      	break;
      case 6: //any games with 4 pieces
        if (sideToMove == ColorWhite) {
          pn[2] = whitePieces[randomNumber(0, 4)]; 
          pn[3] = whitePieces[randomNumber(0, 4)];  
        } else {
          pn[2] = blackPieces[randomNumber(0, 4)];  
          pn[3] = blackPieces[randomNumber(0, 4)];  
        }
        break;
    }
  }
  else if (pieceNumber == 3) {
    switch (gameType) {
      case 0: //3 pawns games with 5 pieces
        pn[2] = pawns[randomNumber(0, 1)];
        pn[3] = pawns[randomNumber(0, 1)];
        pn[4] = pawns[randomNumber(0, 1)];
        break;  
      case 1: //major+2pawns games with 5 pieces
        pn[2] = majors[randomNumber(0, 3)];  
        pn[3] = pawns[randomNumber(0, 1)];
        pn[4] = pawns[randomNumber(0, 1)];
        break;
      case 2: //minor+2pawns games with 5 pieces
        pn[2] = minors[randomNumber(0, 3)];  
        pn[3] = pawns[randomNumber(0, 1)];
        pn[4] = pawns[randomNumber(0, 1)];
        break;
      case 3: //major+minor+pawn games with 5 pieces
        pn[2] = majors[randomNumber(0, 3)];  
        pn[3] = minors[randomNumber(0, 3)];
        pn[4] = pawns[randomNumber(0, 1)];
      	break;
      case 4: //2majors+pawn games with 5 pieces
        pn[2] = majors[randomNumber(0, 3)];  
        pn[3] = majors[randomNumber(0, 3)];
        pn[4] = pawns[randomNumber(0, 1)];
      	break;
      case 5: //2minors+pawn games with 5 pieces
        pn[2] = minors[randomNumber(0, 3)];  
        pn[3] = minors[randomNumber(0, 3)];
        pn[4] = pawns[randomNumber(0, 1)];
      	break;
      case 6: //major+2minors games with 5 pieces
        pn[2] = majors[randomNumber(0, 3)];  
        pn[3] = minors[randomNumber(0, 3)];
        pn[4] = minors[randomNumber(0, 3)];
      	break;
      case 7: //any games with 5 pieces
        if (sideToMove == ColorWhite) {
          pn[2] = whitePieces[randomNumber(0, 4)];
          pn[3] = whitePieces[randomNumber(0, 4)];  
          pn[4] = blackPieces[randomNumber(0, 4)];  
        } else {
          pn[2] = blackPieces[randomNumber(0, 4)];
          pn[3] = blackPieces[randomNumber(0, 4)];  
          pn[4] = whitePieces[randomNumber(0, 4)];  
        }
        break;
    }
  }
  char pieces[255];
  pieces[0] = '\0';
  for (int i = 0; i < pieceNumber + 2; i++) {
  	strncat(pieces, pieceName[pn[i]], strlen(pieceName[pn[i]]));
  	if (i != pieceNumber + 1) strncat(pieces, ", ", 2);
  }
  printf("genEndGame(%u): number of pieces %d: %s\n", threadId, pieceNumber + 2, pieces);
  generateEndGame(pn, pieceNumber + 2, sideToMove, castlingRights, enPassant, board);
}

int playEndGame(struct Engine * chessEngine, struct Board * board, struct Evaluation ** evaluations, FILE * dataSet, bool writedebug) {
  int timeout = 0, result = 0;
  char san_moves[MAX_SAN_MOVES_LEN];
  char full_move[16];
  struct timespec delay;
  delay.tv_sec = 1;
  delay.tv_nsec = 0;

  while (!newGame(chessEngine) && timeout < 3) {
    printf("playEndGame() warning: newGame() returned false\n");
    nanosleep(&delay, NULL);
    timeout++;
  }
  if (!newGame(chessEngine)) {
    printf("playEndGame() error: newGame() returned false, returning error 2 to a caller\n");
  	return 2;
  }
  if (strlen(board->fen->fenString) >= MAX_FEN_STRING_LEN) {
    printf("playEndGame() error: FEN string (%s) is too long (%lu chars), max is %d chars\n", board->fen->fenString, strlen(board->fen->fenString), MAX_FEN_STRING_LEN - 1);
    return 2;
  }
  strncpy(chessEngine->position, board->fen->fenString, MAX_FEN_STRING_LEN);
  chessEngine->moves[0] = '\0';
  san_moves[0] = '\0';
  if (board->fen->sideToMove == ColorBlack) {
    full_move[0] = '\0';
    snprintf(full_move, 16, "%u... ", board->fen->moveNumber);
    strlcat(san_moves, full_move, MAX_SAN_MOVES_LEN);
  }
  while (true) {
    position(chessEngine);
    go(chessEngine, evaluations);
    if (evaluations[0]->bestmove[0] == '\0' || strncmp(evaluations[0]->bestmove, "(none", 5) == 0) {
      printf("playEndGame() error: best move is blank or (none)\n");
      return 2;
    }
    if (writedebug)
      printf("best move %s, score %d, mate in %d, nag %d, hashfull %d, tbhits %d, pv %s\n", evaluations[0]->bestmove, evaluations[0]->scorecp, evaluations[0]->matein, evaluations[0]->nag, evaluations[0]->hashful, evaluations[0]->tbhits, evaluations[0]->pv);
    if (evaluations[0]->nag == 18 || evaluations[0]->nag == 20) result = 1;
    else if (evaluations[0]->nag == 19 || evaluations[0]->nag == 21) result = -1;
    else result = 0;
    strlcat(chessEngine->moves, evaluations[0]->bestmove, MAX_UCI_MOVES_LEN);
    strlcat(chessEngine->moves, " ", MAX_UCI_MOVES_LEN);
    //if (dataSet) fprintf(dataSet, "%s,%s,%d\n", board->fen->fenString, evaluations[0]->bestmove, result);
    if (abs(evaluations[0]->scorecp) < 30 && board->fen->halfmoveClock == 50) {
      result = 0;
      break;
    }
    full_move[0] = '\0';
    struct Move move;
    int res = initMove(&move, board, evaluations[0]->bestmove);
    if (res) return 3;
    if (board->fen->sideToMove == ColorWhite)
      snprintf(full_move, 16, "%u.%s ", board->fen->moveNumber, move.sanMove);
    else snprintf(full_move, 16, "%s ", move.sanMove);
    strlcat(san_moves, full_move, MAX_SAN_MOVES_LEN);    
    makeMove(&move);
    if (writedebug) writeDebug(board, false);
    if (board->isMate) {
      if (board->fen->sideToMove == ColorWhite) {
        result = -1; // Blacks win
        break;
      }
      else {
        result = 1; // Whites win
        break;
      }
    }
    if (board->isStaleMate) {
      result = 0; // draw
      break;
    }
  } //end of while (true)
  char game_result[8];
  size_t len = strlen(san_moves);
  san_moves[len - 1] = '\0';
  switch(result) {
    case -1:
      strcpy(game_result, "0-1");
      strlcat(san_moves, "#", MAX_SAN_MOVES_LEN);
    break;
    case 0:
      strcpy(game_result, "1/2-1/2");
    break;
    case 1:
      strcpy(game_result, "1-0");
      strlcat(san_moves, "#", MAX_SAN_MOVES_LEN);
    break;
  }
  char ymd[11];
  struct tm tm_time;
  time_t t = time(NULL);
  gmtime_r(&t, &tm_time);
  strftime(ymd, 11, "%Y.%m.%d", &tm_time);
  if (dataSet) {
    fprintf(dataSet, "[Event \"Endgame simulations\"]\n");
    fprintf(dataSet, "[Site \"Test site\"]\n");
    fprintf(dataSet, "[Date \"%s\"]\n", ymd);
    fprintf(dataSet, "[Round \"1\"]\n");
    fprintf(dataSet, "[White \"%s\"]\n", chessEngine->id);
    fprintf(dataSet, "[Black \"%s\"]\n", chessEngine->id);
    fprintf(dataSet, "[Result \"%s\"]\n", game_result);
    fprintf(dataSet, "[FEN \"%s\"]\n\n", chessEngine->position);
    fprintf(dataSet, "%s %s\n\n", san_moves, game_result);
  }
  return result;
}

void * genEndGames(void * context) {
  if (!context) {
  	printf("genEndGames(context) error: context is NULL\n");
  	return NULL;
  }
  struct GenEndGamesCtx * ctx = (struct GenEndGamesCtx *)(context); 
  printf("genEndGames(%d): max %d pieces, max %d games\n", ctx->threadId, ctx->maxPieceNumber, ctx->maxGameNumber);
  
  struct Engine * chessEngine = initChessEngine(ctx->engine, ctx->movetime, ctx->depth, ctx->hashSize, ctx->threadNumber, ctx->syzygyPath, ctx->multiPV);
  printf("genEndGames(%d): chessEngine %s, %s\n", ctx->threadId, chessEngine->id, chessEngine->engineName);
  
  struct Evaluation * eval1;
  struct Evaluation * evals[1];
  char filename[255];
  FILE * f;
  
  if (ctx->dataset[0] != '\0') {
  	sprintf(filename, "%s%d.pgn", ctx->dataset, ctx->threadId);
    printf("genEndGames(%d): dataset %s\n", ctx->threadId, filename);
    f = fopen(filename, "a");
    if (!f) {
      printf("genEndGames(%d) error: unable to open dataset file %s\n", ctx->threadId, filename);
      return NULL;
    }
  }
  *(ctx->genEndGamesResult) = 0;
  while (*(ctx->genEndGamesResult) < ctx->maxGameNumber) {
    *(ctx->genEndGamesResult) += 1;
    struct Fen * fen;
    fen = malloc(sizeof(struct Fen));
    struct Board * board;
    board = malloc(sizeof(struct Board));
    board->fen = fen;
    eval1 = malloc(sizeof(struct Evaluation));
    eval1->maxPlies = 16;
    evals[0] = eval1;
    genEndGame(ctx->maxPieceNumber - 2, board, 2, CastlingRightsWhiteNoneBlackNone, FileNone, ctx->threadId);
    if (ctx->writedebug) writeDebug(board, false);
    printf("genEndGames(%d): game number %d, FEN %s\n", ctx->threadId, *(ctx->genEndGamesResult), board->fen->fenString);
    int result = playEndGame(chessEngine, board, (struct Evaluation **)evals, f, ctx->writedebug);
    if (ctx->writedebug) writeDebug(board, false);
    free(eval1);
    free(board);
    free(fen);
    if (result > 1) {
      printf("genEndGames(%d) warning: restarting chess engine %s...\n", ctx->threadId, ctx->engine);
      quit(chessEngine);
      free(chessEngine);
      chessEngine = initChessEngine(ctx->engine, ctx->movetime, ctx->depth, ctx->hashSize, ctx->threadNumber, ctx->syzygyPath, ctx->multiPV);
      printf("genEndGames(%d): chessEngine %s %s\n", ctx->threadId, chessEngine->id, chessEngine->engineName);
      continue;
    }
  }
  fclose(f);
  quit(chessEngine);
  free(chessEngine);
  return ctx->genEndGamesResult;
}

int generateEndGames(int maxGameNumber, int maxPieceNumber, char * dataset, char * engine, long movetime, int depth, int hashSize, int threadNumber, char * syzygyPath, int multiPV, bool writedebug, int threads) {
  int res;
  if (maxPieceNumber > 5) {
  	printf("genearateEndGames() error: at the moment only 5 pieces are supported, given %d\n", maxPieceNumber);
  	return 1;
  }
  init_magic_bitboards();
	struct GenEndGamesCtx * genEndGamesCtx[threads];
	pthread_attr_t attr;
	if ((res = pthread_attr_init(&attr)) != 0) {
  	printf("genearateEndGames(): pthread_attr_init() returned %d\n", res);
  	exit(1);
  }
	pthread_t game_thread_id[threads];
	for (unsigned int t = 0; t < threads; t++) {
		genEndGamesCtx[t] = malloc(sizeof(struct GenEndGamesCtx));
		if (!genEndGamesCtx[t]) {
			printf("genearateEndGames() error: malloc(struct GenEndGamesCtx) returned NULL\n");
      exit(1);		
		}
		genEndGamesCtx[t]->threadId = t;
		genEndGamesCtx[t]->maxGameNumber = maxGameNumber;
		genEndGamesCtx[t]->maxPieceNumber = maxPieceNumber; //two static Kings are already accounted for in genEndGames
		strncpy(genEndGamesCtx[t]->dataset, dataset, 255);
		strncpy(genEndGamesCtx[t]->engine, engine, 255);
		genEndGamesCtx[t]->movetime = movetime;
		genEndGamesCtx[t]->depth = depth;
		genEndGamesCtx[t]->hashSize = hashSize;		
		genEndGamesCtx[t]->threadNumber = threadNumber;
		strncpy(genEndGamesCtx[t]->syzygyPath, syzygyPath, 255);
		genEndGamesCtx[t]->multiPV = multiPV;				
		genEndGamesCtx[t]->writedebug = writedebug;		
		genEndGamesCtx[t]->genEndGamesResult = malloc(sizeof(int));
 		if (genEndGamesCtx[t]->genEndGamesResult == NULL) {
		  printf("genearateEndGames() error: malloc(genEndGamesResult[t]) returned NULL\n");
		  exit(1);
		}
		if ((res = pthread_create(&(game_thread_id[t]), &attr, &genEndGames, (void *)(genEndGamesCtx[t]))) != 0) {
			printf("genearateEndGames(): pthread_create() returned %d\n", res);
		  exit(1);
		}
	}
  if ((res = pthread_attr_destroy(&attr)) != 0) {
	  printf("genearateEndGames(): pthread_attr_destroy() returned %d\n", res);
	  exit(1);
	}
	int numberOfGamesGenerated = 0;
	for (unsigned int t = 0; t < threads; t++) {
		res = pthread_join(game_thread_id[t], (void **)(&(genEndGamesCtx[t]->genEndGamesResult)));
		if (res != 0) {
			printf("genearateEndGames() error: pthread_join[%d] returned %d\n", t, res);
		  exit(1);
		}
		numberOfGamesGenerated += *(genEndGamesCtx[t]->genEndGamesResult);
  	printf("genearateEndGames(%d): generated total number of games so far: %d\n", t, numberOfGamesGenerated);
		free(genEndGamesCtx[t]->genEndGamesResult);
		free(genEndGamesCtx[t]);
	}
	cleanup_magic_bitboards();
  return 0;
}

int nextMoves(unsigned long hash, struct MoveScoreGames ** moves, int sqlThreads) {
  unsigned char q;
	switch (sqlThreads) {
		case 1:
			q = 0; 
		  break;
		case 2:
			q = hash >> 63;
		  break;
		case 4: 
			q = hash >> 62;
		  break;
		case 8: 
			q = hash >> 61;
		  break;
		default:
			printf("nextMoves() error: only 1, 2, 4 and 8 sql threads are supported, provided %d\n", sqlThreads);
			exit(1);
	}
  char filename[15];
  sprintf(filename, "NextMoves%d.db", q);
  sqlite3 * db = openDb(filename, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
  int numberOfMoves = getNextMoves(db, hash, moves);
  closeDb(db);
  return numberOfMoves;
}

int compareScore(void * sideToMove, const void * a, const void * b) {
    enum Color s = *((enum Color *)sideToMove);
    struct MoveScores * s1 = (struct MoveScores *)a;
    struct MoveScores * s2 = (struct MoveScores *)b;
    if (s == ColorWhite) {
      if (s1->score < s2->score) return 1;
      if (s1->score > s2->score) return -1;
    }
    if (s == ColorBlack) {
	    if (s1->score > s2->score) return 1;
	    if (s1->score < s2->score) return -1;    	
    }
    return 0;
}

int bestMoves(unsigned long hash, enum Color sideToMove, struct MoveScores * moves, int sqlThreads) {
	struct MoveScoreGames * moveScoreGames[MAX_NUMBER_OF_NEXT_MOVES];
	int nextMovesNumber = nextMoves(hash, (struct MoveScoreGames **)moveScoreGames, sqlThreads);
  // we need to replace number of games for a given move by total number of games for a given position
  // to get weighted scores such as score = score / totalGames
  int totalGames = 0;
  for (int i = 0; i < nextMovesNumber; i++)
    totalGames += moveScoreGames[i]->games;
  for (int i = 0; i < nextMovesNumber; i++) {
    strncpy(moves[i].move, moveScoreGames[i]->move, 6);
    moves[i].score = (double)moveScoreGames[i]->score / (double)totalGames;
    //printf("%s %f\n", moves[i].move, moves[i].score); 
    free(moveScoreGames[i]);
  }
  //sort the moves from the one with the highest weighted score to the lowest for whites and
  //lowest to highest for blacks
  qsort_r(moves, nextMovesNumber, sizeof(struct MoveScores), &sideToMove, compareScore);
  return nextMovesNumber;
}

///<summary>
/// Plays a given game using Game struct
///</summary>
int playGame(struct Game * game) {
  int numberOfPlies = 0;
	struct Move move;
	struct Board board;
	struct Fen fen;
	char * fenString;
	if (game->tags[FEN][0] == '\0') fenString = startPos;
	else fenString = game->tags[FEN];
	if (strtofen(&fen, fenString)) {
		printf("playGame() error: strtofen() failed; FEN %s\n", fenString);
		return 1;
	}
	if (fentoboard(&fen, &board)) {
		printf("playGame() error: fentoboard() failed; FEN %s\n", fen.fenString);
		return 1;
	}	
	char * sanMoves = strndup(game->sanMoves, strlen(game->sanMoves));
	if (!sanMoves) {
		printf("playGame() error: strndup() returned NULL: %s. sanMoves %s\n", strerror(errno), game->sanMoves);
		return errno;
	}
	char * saveptr;
	char * token = strtok_r(sanMoves, " ", &saveptr);
	while (token) {
		if (initMove(&move, &board, token)) {
			printf("playGame() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
			free(sanMoves);
			return 1;
		}    
		makeMove(&move);
		//reconcile(&board);
		token = strtok_r(NULL, " ", &saveptr);
		numberOfPlies++;
	}
	free(sanMoves);
	if (numberOfPlies != game->numberOfPlies)
	  printf("playGame() error: numberOfPlies (%d) != game->numberOfPlies (%d), SAN moves %s\n", numberOfPlies, game->numberOfPlies, game->sanMoves);
	return 0;
}

///<summary>
/// Plays a given game using Game struct plus do other things
///</summary>
int playGameExt(struct Game * game, bool generateZobristHash, bool updateDb, bool createDataset, FILE * dataset, int threadId, int sqlThreads) {
	struct Move move;
	struct Board board;
	struct ZobristHash zh;//, zh2;
	char gameResult = 0;
	int hashIndex = 0;
	unsigned long hashCache[MAX_NUMBER_OF_GAME_MOVES];
	char uciMoveCache[MAX_NUMBER_OF_GAME_MOVES][6];
	char fenStringCache[MAX_NUMBER_OF_GAME_MOVES][MAX_FEN_STRING_LEN];

	struct Fen fen;
	char * fenString;
	if (game->tags[FEN][0] == '\0') fenString = startPos;
	else fenString = game->tags[FEN];
	if (strtofen(&fen, fenString)) {
		printf("playGameExt(%d) error: strtofen() failed; FEN %s\n", threadId, fenString);
		return 1;
	}
	if (fentoboard(&fen, &board)) {
		printf("playGameExt(%d) error: fentoboard() failed; FEN %s\n", threadId, fen.fenString);
		return 1;
	}

  if (strcmp(game->tags[Result], "1-0") == 0) gameResult = 1;
  else if (strcmp(game->tags[Result], "0-1") == 0) gameResult = -1;
	
	if (generateZobristHash) {
		zobristHash(&zh);
//		zobristHash(&zh2);
		getHash(&zh, &board);
		board.hash = zh.hash;
		//bestMoves() returns sorted array of moves and their scores from the best to the worth
		//from the point of view of the side to move
		//struct MoveScores moves[MAX_NUMBER_OF_NEXT_MOVES];
		//int bestMovesNumber = bestMoves(board.hash, board.fen->sideToMove, moves);
		//for (int i = 0; i < bestMovesNumber; i++)
		//	printf("%s %.8f %s\n", moves[i]->move, moves[i]->score, color[board.fen->sideToMove]);
	}
	
	char * sanMoves = strndup(game->sanMoves, strlen(game->sanMoves));
	if (!sanMoves) {
		printf("playGameExt(%d) error: strndup() returned NULL: %s. sanMoves %s\n", threadId, strerror(errno), game->sanMoves);
		return errno;
	}
	char * saveptr;
	char * token = strtok_r(sanMoves, " ", &saveptr);
	while (token) {
		if (initMove(&move, &board, token)) {
			printf("playGameExt(%d) error: invalid move %u%s%s (%s); FEN %s\n", threadId, move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
			free(sanMoves);
			return 1;
		}
		if (updateDb) {
    	//store the hash and uciMove in the cache to correct gameResult in NextMoves.db
    	//if it happens to be wrong in pgn file

    	if (hashIndex < MAX_NUMBER_OF_GAME_MOVES) {
    	  strncpy(uciMoveCache[hashIndex], move.uciMove, 6);
    	  hashCache[hashIndex++] = board.hash;
    	} else {
    		printf("playGameExt(%d) error: number of moves %d exceeded MAX_NUMBER_OF_GAME_MOVES %d\n", threadId, hashIndex, MAX_NUMBER_OF_GAME_MOVES);
   			free(sanMoves);
    		return 1;
    	}
    }
		if (createDataset) {
    	//store the hash and uciMove in the cache to correct gameResult in NextMoves.db
    	//if it happens to be wrong in pgn file

    	if (hashIndex < MAX_NUMBER_OF_GAME_MOVES || strlen(board.fen->fenString) < MAX_FEN_STRING_LEN) {
    	  strncpy(uciMoveCache[hashIndex], move.uciMove, 6);
    	  strncpy(fenStringCache[hashIndex++], board.fen->fenString, MAX_FEN_STRING_LEN);
    	} else {
    		printf("playGameExt(%d) error: number of moves %d equals or greater than MAX_NUMBER_OF_GAME_MOVES %d or length of FEN string %s equals or greater than MAX_FEN_STRING_LEN %d\n", threadId, hashIndex, MAX_NUMBER_OF_GAME_MOVES, board.fen->fenString, MAX_FEN_STRING_LEN);
   			free(sanMoves);
    		return 1;
    	}
    }
    
		makeMove(&move);
		//reconcile(&board);
		if (generateZobristHash) {
			if (!updateHash(&zh, &board, &move)) {
/*
        //Zobrist Hash calculation in updateHash() verification code
		    getHash(&zh2, &board);
		    if (zh.hash != zh2.hash) {
		    	printf("pgnGame() error: getHash() = %lx != updateHash() = %lx, fen %s\n", zh2.hash, zh.hash, board.fen->fenString);
		    	writeDebug(&board, false);
		    	exit(1);
		    }
*/		    
				board.hash = zh.hash;
				//struct MoveScores moves[MAX_NUMBER_OF_NEXT_MOVES];
				//int bestMovesNumber = bestMoves(board.hash, board.fen->sideToMove, moves);
				//for (int i = 0; i < bestMovesNumber; i++)
				//	printf("%s %.8f %s\n", moves[i].move, moves[i].score, color[board.fen->sideToMove]);
			}
			else {
				printf("playGameExt(%d) error: updateHash() returned non-zero value\n", threadId);
				free(sanMoves);
				return 1;
			}
		}
		if (board.isStaleMate && strcmp(game->tags[Result], "1/2-1/2") != 0) {
			printf("playGameExt(%d): game tag Result '%s' did not match the actual one '1/2-1/2' - corrected in db\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", threadId, game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
			strcpy(game->tags[Result], "1/2-1/2");
			gameResult = 0;
		}
		else if (board.isMate && board.movingPiece.color == ColorWhite && strcmp(game->tags[Result], "1-0") != 0) {
			printf("playGameExt(%d): game tag Result '%s' did not match the actual one '1-0' - corrected in db\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", threadId, game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
			strcpy(game->tags[Result], "1-0");
			gameResult = 1;
		}
		else if (board.isMate && board.movingPiece.color == ColorBlack && strcmp(game->tags[Result], "0-1") != 0) {
			printf("playGameExt(%d): game tag Result '%s' did not match the actual one '0-1' - corrected in db\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", threadId, game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
			strcpy(game->tags[Result], "0-1");
			gameResult = -1;
		}
		token = strtok_r(NULL, " ", &saveptr);
	}
	if (updateDb) {
    struct QueueMoves * queue = NULL;
    unsigned char q = 0;
		for (int i = 0; i < hashIndex; i++) {
    	switch (sqlThreads) {
    		case 1:
    			q = 0; 
    		  break;
    		case 2:
    			q = hashCache[i] >> 63;
    		  break;
    		case 4: 
    			q = hashCache[i] >> 62;
    		  break;
    		case 8: 
    			q = hashCache[i] >> 61;
    		  break;
    		default:
    			printf("playGameExt(%d) error: only 1, 2, 4 and 8 sql threads are supported, provided %d\n", threadId, sqlThreads);
    			exit(1);
    	}
      pthread_mutex_t * mutex = NULL;
			switch (q) {
				case 0:
    		  queue = nextMoves0;
    		  mutex = &queueMoves0_mutex;
					break;
				case 1:
    		  queue = nextMoves1;
    		  mutex = &queueMoves1_mutex;
					break;
				case 2:
    		  queue = nextMoves2;
    		  mutex = &queueMoves2_mutex;
					break;
				case 3:
    		  queue = nextMoves3;
    		  mutex = &queueMoves3_mutex;
					break;
				case 4:
    		  queue = nextMoves4;
    		  mutex = &queueMoves4_mutex;
					break;
				case 5:
    		  queue = nextMoves5;
    		  mutex = &queueMoves5_mutex;
					break;
				case 6:
    		  queue = nextMoves6;
    		  mutex = &queueMoves6_mutex;
					break;
				case 7:
    		  queue = nextMoves7;
    		  mutex = &queueMoves7_mutex;
					break;
				default:
					printf("playGameExt(%d): unknown queue\n", threadId);
					exit(1);
      }
    	pthread_mutex_lock(mutex);
		  enqueueMoves(queue, hashCache[i], uciMoveCache[i], gameResult);
		  pthread_mutex_unlock(mutex);
		}
	}
	if (createDataset && dataset) {
	  /*
		for (int i = 0; i < hashIndex; i++) {
      fprintf(dataset, "%s,%s,%d\n", fenStringCache[i], uciMoveCache[i], gameResult);
    }*/
    fprintf(dataset, "[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n[Result \"%s\"]\n[WhiteElo \"%s\"]\n[BlackElo \"%s\"]\n\n", game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black], game->tags[Result], game->tags[WhiteElo], game->tags[BlackElo]);
    sanMoves = strndup(game->sanMoves, strlen(game->sanMoves));
  	if (!sanMoves) {
  		printf("playGameExt(%d) error: strndup() returned NULL: %s. sanMoves %s\n", threadId, strerror(errno), game->sanMoves);
  		return errno;
  	}
  	token = strtok_r(sanMoves, " ", &saveptr);
  	int moveNumber = 1;
  	bool whiteMove = true;
  	while (token) {
  	  if (whiteMove) {
        fprintf(dataset, "%d.%s ", moveNumber, token);
        whiteMove = false;
      } else {
        fprintf(dataset, "%s ", token);
        whiteMove = true; 
        moveNumber++;       
      }
  		token = strtok_r(NULL, " ", &saveptr);	  
    }
    fprintf(dataset, "%s\n\n", game->tags[Result]);    
	}
	free(sanMoves);
	return 0;
}

int initGame(struct Game * game, FILE * file) {
	char line[80];
	//read game PGN tags
	if (gTags(game->tags, file)) {
	  //fprintf(stderr, "initGame(): gTags() returned EOF\n");
	  return 1; //eof
	} 
	//fprintf(stderr, "initGame(): [Event \"%s\"]\n", game->tags[Event]);

	//skip empty lines and tag lines if any           
	while (fgets(line, sizeof line, file)) {
		if (!isEmptyLine(line) && line[0] != '[') break;
	}
	game->sanMoves[0] = '\0';

	//Read game moves until first empty line
	strcat(game->sanMoves, line);
	while (fgets(line, sizeof line, file)) {
		if (isEmptyLine(line)) break;
		strcat(game->sanMoves, line);
	}
	if (feof(file)) {
	  //fprintf(stderr, "initGame() returned EOF\n");
	  return 1;			
	 }

	//strip the game result;
  stripGameResult(game);
  
	//strip comments, variations and NAGs
	normalizeMoves(game->sanMoves);

	//strip move numbers
	game->numberOfPlies = movesOnly(game->sanMoves);
	return 0;
}

int initGameFromPGNFile(FILE * file, int minElo, int maxEloDiff, int minMoves, bool updateDb, sqlite3 * db, int numberOfEcoLines, struct EcoLine ** ecoLines, bool generateZobristHash, bool createDataset, FILE * dataset, int threadId, int sqlThreads) {
  bool updateNextMoves = false;
  struct Game game;
  
  if (initGame(&game, file)) return 1;

	if (minElo > atoi(game.tags[WhiteElo]) || minElo > atoi(game.tags[BlackElo]) || maxEloDiff < abs(atoi(game.tags[WhiteElo]) - atoi(game.tags[BlackElo])) || minMoves * 2 > game.numberOfPlies) {
		return 2;
  }
 
  if (updateDb) {
    //generate md5 128-bit hash of game.sanMoves, split it in half
    //and XOR both halfs resulting in 64-bit unsigned int used for game de-duplication
    unsigned long hash = md5(game.sanMoves);			  	   
    pthread_mutex_lock(&queueGames_mutex);
    struct GameHash * entry = NULL;
    HASH_FIND(hh, games, (void *)(&hash), 8, entry);
    if (entry || gameExists(db, hash)) {
      updateNextMoves = false;
    }
    else { 
      entry = malloc(sizeof(struct GameHash));
      memcpy(entry->hash, (void *)(&hash), 8);
      HASH_ADD(hh, games, hash, 8, entry);
      updateNextMoves = true;
    }
 		pthread_mutex_unlock(&queueGames_mutex);
  }
	if (numberOfEcoLines) {
		struct Fen fen;
		char * fenString;
		if (game.tags[FEN][0] == '\0') fenString = startPos;
		else fenString = game.tags[FEN];
		if (strtofen(&fen, fenString)) {
			printf("initGameFromPGNFile(%d) error: strtofen() failed; FEN %s\n", threadId, fenString);
			return -1;
		}		
		if (strcmp(game.tags[Variant], "chess 960") != 0 && fen.moveNumber == 1 && fen.sideToMove == ColorWhite)
		  ecoClassify(&game, ecoLines, numberOfEcoLines);
		  //printf("ECO %s\nOpening %s\nVariation %s\n", game.tags[ECO], game.tags[Opening], game.tags[Variation]);
	}
	if (playGameExt(&game, generateZobristHash, updateNextMoves, createDataset, dataset, threadId, sqlThreads)) {
		printf("initGameFromPGNFile(%d) error: playGame() returned non-zero result\n", threadId);
		return -1;
	}
	return 0;
}

void * initGamesFromPGNfile(void * context) {
  int res;
	unsigned long gNumber;
	struct InitGameFromPGNfileContext * ctx = (struct InitGameFromPGNfileContext *)context;
  FILE * dataset = NULL;
  
	FILE * file = fopen((char *)ctx->fileName, "r");
	if (!file) {
		printf("initGamesFromPGNfile(%d) error: failed to open a file %s, %s\n", ctx->threadNumber, (char *)ctx->fileName, strerror(errno));
		*(ctx->initGameResult) = -1;
		return ctx->initGameResult;
	}
	if (ctx->createDataset) {
		char filename[255];
		sprintf(filename, "%s%d.pgn", ctx->dataset, ctx->threadNumber);
		dataset = fopen(filename, "a");
		if (!dataset) {
			printf("initGamesFromPGNfile(%d) error: failed to open/create a file %s, %s\n", ctx->threadNumber, filename, strerror(errno));
			*(ctx->initGameResult) = -1;
			return ctx->initGameResult;
		}
	}

	*(ctx->initGameResult) = 0;

	while (true) {
		pthread_mutex_lock(&gameNumber_mutex);
		gNumber = gameNumber++;
		pthread_mutex_unlock(&gameNumber_mutex);
		if (gNumber >= numberOfGames) break;
		fseek(file, gameStartPositions[gNumber], SEEK_SET);
		res = initGameFromPGNFile(file, ctx->minElo, ctx->maxEloDiff, ctx->minMoves, ctx->updateDb, ctx->db, ctx->numberOfEcoLines, ctx->ecoLines, ctx->generateZobristHash, ctx->createDataset, dataset, ctx->threadNumber, ctx->sqlThreads);
		if (res < 0) {
	    printf("initGamesFromPGNfile(%d) exited with the result %d, file %s\n", ctx->threadNumber, res, ctx->fileName);
		  *(ctx->initGameResult) = -1;
		  break;
	  }
	  else if (res == 1) {
      printf("initGamesFromPGNfile(%d): end of file %s\n", ctx->threadNumber, ctx->fileName);
	  	break;
	  }
	  else if (res == 2) {
	  	continue;
	  }
    else {
      *(ctx->initGameResult) += 1;
      if (*(ctx->initGameResult) >= ctx->numberOfGames) break;
    }
	}
	fclose(file);
	if (ctx->createDataset) fclose(dataset);

	return ctx->initGameResult;
}

void * initGamesFromPGNfiles(void * context) {
  bool updateDb = false;
  int fNumber, res;

	struct InitGameFromPGNfilesContext * ctx = (struct InitGameFromPGNfilesContext *)context;
	*(ctx->initGameResult) = 0;
	FILE * dataset = NULL;
	if (ctx->createDataset) {
		char filename[255];
		sprintf(filename, "%s%d.pgn", ctx->dataset, ctx->threadNumber);
		dataset = fopen(filename, "a");
		if (!dataset) {
			printf("initGamesFromPGNfiles(%d) error: failed to open/create a file %s, %s\n", ctx->threadNumber, filename, strerror(errno));
			*(ctx->initGameResult) = -1;
			return ctx->initGameResult;
		}
	}

	while (true) {
		pthread_mutex_lock(&fileNumber_mutex);
		fNumber = fileNumber++;
		pthread_mutex_unlock(&fileNumber_mutex);
		if (fNumber >= ctx->numberOfFiles) break;
		
		FILE * file = fopen(ctx->fileNames[fNumber], "r");
		if (!file) {
			printf("initGamesFromPGNfiles() error: failed to open a file %s, %s\n", ctx->fileNames[fNumber], strerror(errno));
			*(ctx->initGameResult) = -1;
			return ctx->initGameResult;
		}
		while (true) {
			res = initGameFromPGNFile(file, ctx->minElo, ctx->maxEloDiff, ctx->minMoves, ctx->updateDb, ctx->db, ctx->numberOfEcoLines, ctx->ecoLines, ctx->generateZobristHash, ctx->createDataset, dataset, ctx->threadNumber, ctx->sqlThreads);
			if (res < 0) {
		    printf("initGameFromPGNfile(%d) exited with the result %d, file %s\n", ctx->threadNumber, res, ctx->fileNames[fNumber]);
			  *(ctx->initGameResult) = -1;
			  break;
		  }
		  else if (res == 1) {
	      printf("initGameFromPGNfile(%d): end of file %s\n", ctx->threadNumber, ctx->fileNames[fNumber]);
		  	break;
		  }
		  else if (res == 2) {
		    continue;
		  }
	    else {
	      *(ctx->initGameResult) += 1;
	      if (*(ctx->initGameResult) >= ctx->numberOfGames) break;
	    }
		}
  	fclose(file);
	}
	if (ctx->createDataset) fclose(dataset);
	return ctx->initGameResult;
}

void * sqlWriterMoves(void * context) {
	struct timespec delay;
  delay.tv_sec = 1;
  delay.tv_nsec = 0;

  unsigned long hash, rowNumber = 0, queueLen = 0;
  char nextMove[6];
  char score;
  char * errmsg = NULL;
  int sleepCounter = 0;
  struct SqlContext * ctx = NULL;
  if (context)
    ctx = (struct SqlContext *)context;
  else {
  	printf("sqlWriterMoves(context): context is NULL\n");
  	exit(1);
  }
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  char filename[15];
  sprintf(filename, "NextMoves%d.db", ctx->threadNumber);
  sqlite3 * db = openDb(filename, flags);
  const char * sql = "create table if not exists next_moves (hash integer, next_move varchar(7), score int, games int, constraint hash_move primary key (hash, next_move) on conflict ignore) without rowid;";
  int res = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
  if (res != SQLITE_OK) {
     printf("sqlWriterMoves() error: sqlite3_exec(create table if not exists next_moves...) returns %d (%s): %s, ext err code %d \n", res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
     sqlite3_free(errmsg);
     sqlite3_close_v2(db);
     exit(1);
  }
  
  res = sqlite3_exec(db, "begin transaction;", NULL, NULL, &errmsg);
  if (res != SQLITE_OK) {
     printf("sqlWriterMoves() error: sqlite3_exec(begin transaction;) returns %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
     sqlite3_free(errmsg);
     sqlite3_close_v2(db);
     exit(1);
  }
  
  while(sleepCounter < MAX_SLEEP_COUNTER_FOR_SQLWRITER) {
    pthread_mutex_lock(ctx->mutex);
    queueLen = ctx->queue->len;
    //printf("nextMoves len is %d\n", nextMoves->len);
    if (queueLen > 0) {
      hash = ctx->queue->first->hash;
      score = ctx->queue->first->score;
    	strncpy(nextMove, ctx->queue->first->nextMove, 6);
		  dequeueMoves(ctx->queue);
  		pthread_mutex_unlock(ctx->mutex);
  		rowNumber++;
    } else {
  		pthread_mutex_unlock(ctx->mutex);
  		printf("sqlWriterMoves(%d): queue is empty. sleepCounter %d, delay %ld sec\n", ctx->threadNumber, sleepCounter, delay.tv_sec);
	    if (rowNumber > 0) {
	    	printf("sqlWriterMoves(%d): committing %lu rows to next_moves. Queue len %lu\n", ctx->threadNumber, rowNumber, queueLen);
			  res = sqlite3_exec(db, "commit transaction;", NULL, NULL, &errmsg);
			  if (res != SQLITE_OK) {
			    printf("sqlWriterMoves(%d) sqlite3_exec(commit transaction;) returns %d (%s): %s, ext err code %d\n", ctx->threadNumber, res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
			    sqlite3_free(errmsg);
			    sqlite3_close_v2(db);
			    exit(1);
			  }
			  rowNumber = 0;
		    res = sqlite3_exec(db, "begin transaction;", NULL, NULL, &errmsg);
			  if (res != SQLITE_OK) {
			    printf("sqlWriterMoves(%d) error: sqlite3_exec(begin transaction;) returns %d (%s): %s, ext err code %d\n", ctx->threadNumber, res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
			    sqlite3_free(errmsg);
			    sqlite3_close_v2(db);
			    exit(1);
			  }
			} else {
	    	delay.tv_sec += 1;
			  sleepCounter++;
		  }
			nanosleep(&delay, NULL);
			continue;
    }
    updateNextMove(db, hash, nextMove, score);
    if (rowNumber >= COMMIT_NEXT_MOVES_ROWS) {
    	printf("sqlWriterMoves(%d): committing %u rows to next_moves. Queue len %lu\n", ctx->threadNumber, COMMIT_NEXT_MOVES_ROWS, queueLen);
		  res = sqlite3_exec(db, "commit transaction;", NULL, NULL, &errmsg);
		  if (res != SQLITE_OK) {
		    printf("sqlWriterMoves(%d) sqlite3_exec(commit transaction;) returns %d (%s): %s, ext err code %d\n", ctx->threadNumber, res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
		    sqlite3_free(errmsg);
		    sqlite3_close_v2(db);
		    exit(1);
		  }
		  rowNumber = 0;
	    res = sqlite3_exec(db, "begin transaction;", NULL, NULL, &errmsg);
		  if (res != SQLITE_OK) {
		    printf("sqlWriterMoves(%d) error: sqlite3_exec(begin transaction;) returns %d (%s): %s, ext err code %d\n", ctx->threadNumber, res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
		    sqlite3_free(errmsg);
		    sqlite3_close_v2(db);
		    exit(1);
		  }
		}
  }
  res = sqlite3_exec(db, "commit transaction;", NULL, NULL, &errmsg);
  if (res != SQLITE_OK) {
    printf("sqlWriterMoves(%d) sqlite3_exec(commit transaction;) returns %d (%s): %s, ext err code %d\n", ctx->threadNumber, res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
    sqlite3_free(errmsg);
    sqlite3_close_v2(db);
    exit(1);
  }
  closeDb(db);
  return NULL;
}

void * sqlWriterGames(void * context) {
  int res;
  char * errmsg = NULL;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
  sqlite3 * db = openDb("Games.db", flags);
  
  res = sqlite3_exec(db, "begin transaction;", NULL, NULL, &errmsg);
  if (res != SQLITE_OK) {
     printf("sqlWriterGames() error: sqlite3_exec(begin transaction;) returns %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
     sqlite3_free(errmsg);
     sqlite3_close_v2(db);
     exit(1);
  }
  
  pthread_mutex_lock(&queueGames_mutex);
  //BTreeDestroy(games, db); //this will insert md5 hash into games table if it's not there
  struct GameHash * entry = NULL, * tmp = NULL;
  HASH_ITER(hh, games, entry, tmp) {
    if (db) insertGame(db, (unsigned long)(*(entry->hash)), NULL);
    HASH_DEL(games, entry);
    free(entry);
  }
  games = NULL;
  pthread_mutex_unlock(&queueGames_mutex);

  res = sqlite3_exec(db, "commit transaction;", NULL, NULL, &errmsg);
  if (res != SQLITE_OK) {
    printf("sqlWriterGames() sqlite3_exec(commit transaction;) returns %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
    sqlite3_free(errmsg);
    sqlite3_close_v2(db);
    exit(1);
  }
  closeDb(db);
  return NULL;
}

///<summary>
/// plays multiple pgn games from a given signle pgn file
/// second arguments is eco file name for ECO classification
/// returns the number of games or -1 in case of an error
/// this implements multi-threading process where each thread plays
/// a single game
///</summary>
unsigned long openGamesFromPGNfile(char * fileName, int gameThreads, int sqlThreads, char * ecoFileName, int minElo, int maxEloDiff, int minMoves, int games, bool generateZobristHash, bool updateDb, bool createDataset, char * dataset) {
	int res = 0, numberOfGamesPlayed = 0, numberOfEcoLines = 0;
	sqlite3 * db = NULL;
	struct EcoLine * ecoLines[MAX_NUMBER_OF_ECO_LINES];

	gameNumber = 0;
	init_magic_bitboards();
	
	FILE * file = fopen(fileName, "r");
	if (!file) {
		printf("countGames() error: failed to open a file %s, %s\n", fileName, strerror(errno));
		return 0;
	}
	
	if (ecoFileName) {
		numberOfEcoLines = initEcoLines(ecoFileName, (struct EcoLine **)ecoLines);
		if (!numberOfEcoLines) printf("openGamesFromPGNfile() warning: initEcoLines(%s) returned 0\n", ecoFileName);
  }
  
  if (updateDb) {
	  char * errmsg = NULL;
	  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
	  db = openDb("Games.db", flags);
	  const char * sql = "create table if not exists games (hash integer, constraint hash_games primary key (hash) on conflict ignore) without rowid;";
	  int res = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
	  if (res != SQLITE_OK) {
	     printf("openGamesFromPGNfile() error: sqlite3_exec(create table if not exists games...) returns %d (%s): %s, ext err code %d \n", res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
	     sqlite3_free(errmsg);
	     sqlite3_close_v2(db);
	     exit(1);
	  }  	
  } 
   
	numberOfGames = countGames(file, "[Event ", gameStartPositions, MAX_NUMBER_OF_GAMES);
	//printf("countGames returned %lu\n", numberOfGames);
	fclose(file);

	if (numberOfGames > MAX_NUMBER_OF_GAMES) {
		printf("openGamesFromPGNfile() warning: number of games in a pgn file %s is %lu, which is greater than the maximum %d\n", fileName, numberOfGames, MAX_NUMBER_OF_GAMES);
		numberOfGames = MAX_NUMBER_OF_GAMES;
	}

	struct InitGameFromPGNfileContext * initGameCtx[gameThreads];
	struct SqlContext * sqlCtx[sqlThreads];
	if (updateDb) {
	  switch (sqlThreads) {
	  	case 8:
				nextMoves7 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves7) {
					printf("openGamesFromPGNfile() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves7->first = nextMoves7->last = NULL;
			  nextMoves7->len = 0;
	
				nextMoves6 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves6) {
					printf("openGamesFromPGNfile() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves6->first = nextMoves6->last = NULL;
			  nextMoves6->len = 0;
				
				nextMoves5 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves5) {
					printf("openGamesFromPGNfile() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves5->first = nextMoves5->last = NULL;
			  nextMoves5->len = 0;
				
				nextMoves4 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves4) {
					printf("openGamesFromPGNfile() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves4->first = nextMoves4->last = NULL;
			  nextMoves4->len = 0;
	  	case 4:
				nextMoves3 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves3) {
					printf("openGamesFromPGNfile() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves3->first = nextMoves3->last = NULL;
			  nextMoves3->len = 0;
	
				nextMoves2 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves2) {
					printf("openGamesFromPGNfile() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves2->first = nextMoves2->last = NULL;
			  nextMoves2->len = 0;
	  	case 2:
				nextMoves1 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves1) {
					printf("openGamesFromPGNfile() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves1->first = nextMoves1->last = NULL;
			  nextMoves1->len = 0;
	  	case 1:
				nextMoves0 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves0) {
					printf("openGamesFromPGNfile() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves0->first = nextMoves0->last = NULL;
			  nextMoves0->len = 0;
			  break;
			default:
				printf("openGamesFromPGNfile: only 1, 2, 4 and 8 sql threads are supported, provided %d\n", sqlThreads);
				exit(1);
	  }
  }
	//Multi-threading implementation, where each thread will 
	//sequentially process one file from a list at a time
  
	pthread_attr_t attr;
	if ((res = pthread_attr_init(&attr)) != 0) {
  	printf("openGamesFromPGNfile: pthread_attr_init() returned %d\n", res);
  	exit(1);
  }

	//Multi-threading implementation, where each thread will process one game at a time
	//locking, updating and unlocking the current game number
  
	pthread_t game_thread_id[gameThreads];
	for (unsigned int t = 0; t < gameThreads; t++) {
		initGameCtx[t] = malloc(sizeof(struct InitGameFromPGNfileContext));
		if (!initGameCtx[t]) {
			printf("openGamesFromPGNfile() error: malloc(struct InitGameFromPGNfileContext) returned NULL\n");
			exit(1);
		}
		strncpy(initGameCtx[t]->fileName, fileName, 255);
		initGameCtx[t]->generateZobristHash = generateZobristHash;
		initGameCtx[t]->threadNumber = t;
		initGameCtx[t]->minElo = minElo;
		initGameCtx[t]->maxEloDiff = maxEloDiff;
		initGameCtx[t]->minMoves = minMoves;
		initGameCtx[t]->numberOfGames = games;
		initGameCtx[t]->numberOfEcoLines = numberOfEcoLines;
		initGameCtx[t]->ecoLines = (struct EcoLine **)ecoLines;
		initGameCtx[t]->updateDb = updateDb;
		initGameCtx[t]->createDataset = createDataset;
		strncpy(initGameCtx[t]->dataset, dataset, 255);
		initGameCtx[t]->db = db;
		initGameCtx[t]->sqlThreads = sqlThreads;
 		initGameCtx[t]->initGameResult = malloc(sizeof(int));
 		if (initGameCtx[t]->initGameResult == NULL) {
		  printf("openGamesFromPGNfile() error: malloc(initGameResult[t]) returned NULL\n");
		  exit(1);
		}
		if ((res = pthread_create(&(game_thread_id[t]), &attr, &initGamesFromPGNfile, (void *)(initGameCtx[t]))) != 0) {
			printf("openGamesFromPGNfile(): pthread_create() returned %d\n", res);
		  exit(1);
		}
	}
	
	pthread_t sql_thread_id[sqlThreads + 1]; //an extra sql thread is 
		//for writing sanMoves MD5 hashes in Games.db for game de-duplication
	if (updateDb) {
		for (unsigned int t = 0; t < sqlThreads; t++) {
			sqlCtx[t] = malloc(sizeof(struct SqlContext));
			if (!sqlCtx[t]) {
				printf("openGamesFromPGNfile() error: malloc(struct SqlContext) returned NULL\n");
	      exit(1);		
			}
			sqlCtx[t]->threadNumber = t;
			switch (t) {
			  case 0:
			  	sqlCtx[0]->queue = nextMoves0;
			  	res = pthread_mutex_init(&queueMoves0_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfile() error: pthread_mutex_init(&queueMoves0_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[0]->mutex = &queueMoves0_mutex;
			  	break;
			  case 1:
			  	sqlCtx[1]->queue = nextMoves1;
			  	res = pthread_mutex_init(&queueMoves1_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfile() error: pthread_mutex_init(&queueMoves1_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[1]->mutex = &queueMoves1_mutex;
			  	break;
			  case 2:
			  	sqlCtx[2]->queue = nextMoves2;
			  	res = pthread_mutex_init(&queueMoves2_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfile() error: pthread_mutex_init(&queueMoves2_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[2]->mutex = &queueMoves2_mutex;
			  	break;
			  case 3:
			  	sqlCtx[3]->queue = nextMoves3;
			  	res = pthread_mutex_init(&queueMoves3_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfile() error: pthread_mutex_init(&queueMoves3_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[3]->mutex = &queueMoves3_mutex;
			  	break;
			  case 4:
			  	sqlCtx[4]->queue = nextMoves4;
			  	res = pthread_mutex_init(&queueMoves4_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfile() error: pthread_mutex_init(&queueMoves4_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[4]->mutex = &queueMoves4_mutex;
			  	break;
			  case 5:
			  	sqlCtx[5]->queue = nextMoves5;
			  	res = pthread_mutex_init(&queueMoves5_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfile() error: pthread_mutex_init(&queueMoves5_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[5]->mutex = &queueMoves5_mutex;
			  	break;
			  case 6:
			  	sqlCtx[6]->queue = nextMoves6;
			  	res = pthread_mutex_init(&queueMoves6_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfile() error: pthread_mutex_init(&queueMoves6_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[6]->mutex = &queueMoves6_mutex;
			  	break;
			  case 7:
			  	sqlCtx[7]->queue = nextMoves7;
			  	res = pthread_mutex_init(&queueMoves7_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfile() error: pthread_mutex_init(&queueMoves7_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[7]->mutex = &queueMoves7_mutex;
			  	break;
			  default:
			  	printf("openGamesFromPGNfile() error: only 1, 2, 4 and 8 sql threads are supported\n");
			  	exit(1);
			}
			if ((res = pthread_create(&(sql_thread_id[t]), &attr, &sqlWriterMoves,   (void *)(sqlCtx[t]))) != 0) {
				printf("openGamesFromPGNfile(): pthread_create(sqlWriterMoves) returned %d\n", res);
			  exit(1);
			}
	  }
	}
	for (unsigned int t = 0; t < gameThreads; t++) {
		res = pthread_join(game_thread_id[t], (void **)(&(initGameCtx[t]->initGameResult)));
		if (res != 0) {
			printf("openGamesFromPGNfile() error: pthread_join[%d] returned %d\n", t, res);
		  exit(1);
		}
		if (*(initGameCtx[t]->initGameResult) > 0) numberOfGamesPlayed += *(initGameCtx[t]->initGameResult);
  	printf("openGamesFromPGNfile(%d): number of games played so far: %d\n", t, numberOfGamesPlayed);
		free(initGameCtx[t]->initGameResult);
		free(initGameCtx[t]);
	}
	if (updateDb) {
		if ((res = pthread_create(&(sql_thread_id[sqlThreads]), &attr, &sqlWriterGames, NULL) != 0)) {
			printf("openGamesFromPGNfile(): pthread_create(sqlWriterGames) returned %d\n", res);
			exit(1);
		}
		res = pthread_join(sql_thread_id[sqlThreads], NULL);
		if (res != 0) {
			printf("openGamesFromPGNfile() error: pthread_join[%d] returned %d\n", sqlThreads, res);
			exit(1);
		}
		for (unsigned int t = 0; t < sqlThreads; t++) {
			res = pthread_join(sql_thread_id[t], NULL);
			if (res != 0) {
				printf("openGamesFromPGNfile() error: pthread_join[%d] returned %d\n", t, res);
			  exit(1);
			}
	   	printf("openGamesFromPGNfile(): sql thread %d of %d has finished\n", t, sqlThreads);
			free(sqlCtx[t]);
		}
  }
  if ((res = pthread_attr_destroy(&attr)) != 0) {
	  printf("openGamesFromPGNfile(): pthread_attr_destroy() returned %d\n", res);
	  exit(1);
	}
		
	for (int i = 0; i < numberOfEcoLines; i++) free(ecoLines[i]);
	if (updateDb) {
		switch (sqlThreads) {
	  	case 8:
	    	free(nextMoves7);
	    	free(nextMoves6);
	    	free(nextMoves5);
	    	free(nextMoves4);
	    	pthread_mutex_destroy(&queueMoves7_mutex);
	    	pthread_mutex_destroy(&queueMoves6_mutex);
	    	pthread_mutex_destroy(&queueMoves5_mutex);
	    	pthread_mutex_destroy(&queueMoves4_mutex);
	  	case 4:
	    	free(nextMoves3);
	    	free(nextMoves2);
	    	pthread_mutex_destroy(&queueMoves3_mutex);
	    	pthread_mutex_destroy(&queueMoves2_mutex);
	  	case 2:
	    	free(nextMoves1);
	    	pthread_mutex_destroy(&queueMoves2_mutex);
	  	case 1:
	    	free(nextMoves0);
	    	pthread_mutex_destroy(&queueMoves0_mutex);
	  }
	  closeDb(db);
  }
	cleanup_magic_bitboards();
	if (res) return res;
	return numberOfGamesPlayed;
}

///<summary>
/// This function is similar to playGames() with a difference that it takes a list of PGN
/// files and each thread is given the entire file from the list
/// It saves time by begin playing games one by one from the start of a file
/// eliminating the need to index games in the file first, which is time consuming
///</summary>
unsigned long openGamesFromPGNfiles(char * fileNames[], int numberOfFiles, int gameThreads, int sqlThreads, char * ecoFileName, int minElo, int maxEloDiff, int minMoves, int numberOfGames, bool generateZobristHash, bool updateDb, bool createDataset, char * dataset) {
	int res = 0, numberOfGamesPlayed = 0, numberOfEcoLines = 0;;
	struct EcoLine * ecoLines[MAX_NUMBER_OF_ECO_LINES];
	sqlite3 * db = NULL;
	
	init_magic_bitboards();
	if (ecoFileName) {
		numberOfEcoLines = initEcoLines(ecoFileName, (struct EcoLine **)ecoLines);
		if (!numberOfEcoLines) {
			printf("openGamesFromPGNfiles() warning: initEcoLines(%s) returned 0\n", ecoFileName);
		  exit(1);
		}
  }
  
  if (updateDb) {
	  char * errmsg = NULL;
	  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
	  db = openDb("Games.db", flags);
	  const char * sql = "create table if not exists games (hash integer, constraint hash_games primary key (hash) on conflict ignore) without rowid;";
	  int res = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
	  if (res != SQLITE_OK) {
	     printf("openGamesFromPGNfiles() error: sqlite3_exec(create table if not exists games...) returns %d (%s): %s, ext err code %d \n", res, sqlite3_errstr(res), errmsg, sqlite3_extended_errcode(db));
	     sqlite3_free(errmsg);
	     sqlite3_close_v2(db);
	     exit(1);
	  }  	
  } 
  
	struct InitGameFromPGNfilesContext * initGameCtx[gameThreads];
	struct SqlContext * sqlCtx[sqlThreads];
	if (updateDb) {
	  switch (sqlThreads) {
	  	case 8:
				nextMoves7 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves7) {
					printf("openGamesFromPGNfiles() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves7->first = nextMoves7->last = NULL;
			  nextMoves7->len = 0;
	
				nextMoves6 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves6) {
					printf("openGamesFromPGNfiles() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves6->first = nextMoves6->last = NULL;
			  nextMoves6->len = 0;
				
				nextMoves5 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves5) {
					printf("openGamesFromPGNfiles() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves5->first = nextMoves5->last = NULL;
			  nextMoves5->len = 0;
				
				nextMoves4 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves4) {
					printf("openGamesFromPGNfiles() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves4->first = nextMoves4->last = NULL;
			  nextMoves4->len = 0;
	  	case 4:
				nextMoves3 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves3) {
					printf("openGamesFromPGNfiles() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves3->first = nextMoves3->last = NULL;
			  nextMoves3->len = 0;
	
				nextMoves2 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves2) {
					printf("openGamesFromPGNfiles() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves2->first = nextMoves2->last = NULL;
			  nextMoves2->len = 0;
	  	case 2:
				nextMoves1 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves1) {
					printf("openGamesFromPGNfiles() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves1->first = nextMoves1->last = NULL;
			  nextMoves1->len = 0;
	  	case 1:
				nextMoves0 = malloc(sizeof(struct QueueMoves));
				if (!nextMoves0) {
					printf("openGamesFromPGNfiles() error: malloc(QueueMoves) returned NULL for nextMoves\n");
					exit(1);
				}
				nextMoves0->first = nextMoves0->last = NULL;
			  nextMoves0->len = 0;
			  break;
			default:
				printf("openGamesFromPGNfiles(): only 1, 2, 4 and 8 sql threads are supported, provided %d\n", sqlThreads);
				exit(1);
	  }
  }
	//Multi-threading implementation, where each thread will 
	//sequentially process one file from a list at a time
  
	pthread_attr_t attr;
	if ((res = pthread_attr_init(&attr)) != 0) {
  	printf("openGamesFromPGNfiles(): pthread_attr_init() returned %d\n", res);
  	exit(1);
  }
	pthread_t game_thread_id[gameThreads];
	for (unsigned int t = 0; t < gameThreads; t++) {
		initGameCtx[t] = malloc(sizeof(struct InitGameFromPGNfilesContext));
		if (!initGameCtx[t]) {
			printf("openGamesFromPGNfiles() error: malloc(struct PlayGameContext) returned NULL\n");
      exit(1);		
		}
		for (int i = 0; i < numberOfFiles; i++) {
		  strncpy(initGameCtx[t]->fileNames[i], fileNames[i], 255);
		}
		initGameCtx[t]->generateZobristHash = generateZobristHash;
		initGameCtx[t]->numberOfFiles = numberOfFiles;
		initGameCtx[t]->threadNumber = t;
		initGameCtx[t]->minElo = minElo;
		initGameCtx[t]->maxEloDiff = maxEloDiff;
		initGameCtx[t]->minMoves = minMoves;
		initGameCtx[t]->numberOfGames = numberOfGames;
		initGameCtx[t]->numberOfEcoLines = numberOfEcoLines;
		initGameCtx[t]->ecoLines = (struct EcoLine **)ecoLines;		
		initGameCtx[t]->updateDb = updateDb;
		initGameCtx[t]->createDataset = createDataset;
		strncpy(initGameCtx[t]->dataset, dataset, 255);
		initGameCtx[t]->db = db;
		initGameCtx[t]->sqlThreads = sqlThreads;
		initGameCtx[t]->initGameResult = malloc(sizeof(int));
 		if (initGameCtx[t]->initGameResult == NULL) {
		  printf("openGamesFromPGNfiles() error: malloc(initGameResult[t]) returned NULL\n");
		  exit(1);
		}
		if ((res = pthread_create(&(game_thread_id[t]), &attr, &initGamesFromPGNfiles, (void *)(initGameCtx[t]))) != 0) {
			printf("openGamesFromPGNfiles(): pthread_create() returned %d\n", res);
		  exit(1);
		}
	}

	pthread_t sql_thread_id[sqlThreads + 1]; //an extra sql thread is 
		                                    //for writing sanMoves MD5 hashes in Games.db for game de-duplication
  if (updateDb) {
		for (unsigned int t = 0; t < sqlThreads; t++) {
			sqlCtx[t] = malloc(sizeof(struct SqlContext));
			if (!sqlCtx[t]) {
				printf("openGamesFromPGNfiles() error: malloc(struct SqlContext) returned NULL\n");
	      exit(1);		
			}
			sqlCtx[t]->threadNumber = t;
			switch (t) {
			  case 0:
			  	sqlCtx[0]->queue = nextMoves0;
			  	res = pthread_mutex_init(&queueMoves0_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfiles() error: pthread_mutex_init(&queueMoves0_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[0]->mutex = &queueMoves0_mutex;
			  	break;
			  case 1:
			  	sqlCtx[1]->queue = nextMoves1;
			  	res = pthread_mutex_init(&queueMoves1_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfiles() error: pthread_mutex_init(&queueMoves1_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[1]->mutex = &queueMoves1_mutex;
			  	break;
			  case 2:
			  	sqlCtx[2]->queue = nextMoves2;
			  	res = pthread_mutex_init(&queueMoves2_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfiles() error: pthread_mutex_init(&queueMoves2_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[2]->mutex = &queueMoves2_mutex;
			  	break;
			  case 3:
			  	sqlCtx[3]->queue = nextMoves3;
			  	res = pthread_mutex_init(&queueMoves3_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfiles() error: pthread_mutex_init(&queueMoves3_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[3]->mutex = &queueMoves3_mutex;
			  	break;
			  case 4:
			  	sqlCtx[4]->queue = nextMoves4;
			  	res = pthread_mutex_init(&queueMoves4_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfiles() error: pthread_mutex_init(&queueMoves4_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[4]->mutex = &queueMoves4_mutex;
			  	break;
			  case 5:
			  	sqlCtx[5]->queue = nextMoves5;
			  	res = pthread_mutex_init(&queueMoves5_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfiles() error: pthread_mutex_init(&queueMoves5_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[5]->mutex = &queueMoves5_mutex;
			  	break;
			  case 6:
			  	sqlCtx[6]->queue = nextMoves6;
			  	res = pthread_mutex_init(&queueMoves6_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfiles() error: pthread_mutex_init(&queueMoves6_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[6]->mutex = &queueMoves6_mutex;
			  	break;
			  case 7:
			  	sqlCtx[7]->queue = nextMoves7;
			  	res = pthread_mutex_init(&queueMoves7_mutex, NULL);
			  	if (res) {
			  		printf("openGamesFromPGNfiles() error: pthread_mutex_init(&queueMoves7_mutex) returned %d\n", res);
			  		exit(1);
			  	}
			  	sqlCtx[7]->mutex = &queueMoves7_mutex;
			  	break;
			  default:
			  	printf("openGamesFromPGNfiles() error: only 1, 2, 4 and 8 sql threads are supported\n");
			  	exit(1);
			}
			if ((res = pthread_create(&(sql_thread_id[t]), &attr, &sqlWriterMoves,  (void *)(sqlCtx[t]))) != 0) {
				printf("openGamesFromPGNfiles(): pthread_create(sqlWriterMoves) returned %d\n", res);
			  exit(1);
			}
	  }
	}
	
	for (unsigned int t = 0; t < gameThreads; t++) {
		res = pthread_join(game_thread_id[t], (void **)(&(initGameCtx[t]->initGameResult)));
		if (res != 0) {
			printf("openGamesFromPGNfiles() error: pthread_join[%d] returned %d\n", t, res);
		  exit(1);
		}
		if (*(initGameCtx[t]->initGameResult) > 0) numberOfGamesPlayed += *(initGameCtx[t]->initGameResult);
  	printf("openGamesFromPGNfiles(%d): number of games played so far: %d\n", t, numberOfGamesPlayed);
		free(initGameCtx[t]->initGameResult);
		free(initGameCtx[t]);
	}
	if (updateDb) {
		if ((res = pthread_create(&(sql_thread_id[sqlThreads]), &attr, &sqlWriterGames, NULL) != 0)) {
			printf("openGamesFromPGNfiles(): pthread_create(sqlWriterGames) returned %d\n", res);
			exit(1);
		}
		res = pthread_join(sql_thread_id[sqlThreads], NULL);
		if (res != 0) {
			printf("openGamesFromPGNfiles() error: pthread_join[%d] returned %d\n", sqlThreads, res);
			exit(1);
		}
		for (unsigned int t = 0; t < sqlThreads; t++) {
			res = pthread_join(sql_thread_id[t], NULL);
			if (res != 0) {
				printf("openGamesFromPGNfiles() error: pthread_join[%d] returned %d\n", t, res);
			  exit(1);
			}
	   	printf("openGamesFromPGNfiles(): sql thread %d of %d has finished\n", t, sqlThreads);
			free(sqlCtx[t]);
		}
	}
	
  if ((res = pthread_attr_destroy(&attr)) != 0) {
	  printf("openGamesFromPGNfiles(): pthread_attr_destroy() returned %d\n", res);
	  exit(1);
	}
		
	for (int i = 0; i < numberOfEcoLines; i++) free(ecoLines[i]);
	
	if (updateDb) {
		switch (sqlThreads) {
	  	case 8:
	    	free(nextMoves7);
	    	free(nextMoves6);
	    	free(nextMoves5);
	    	free(nextMoves4);
	    	pthread_mutex_destroy(&queueMoves7_mutex);
	    	pthread_mutex_destroy(&queueMoves6_mutex);
	    	pthread_mutex_destroy(&queueMoves5_mutex);
	    	pthread_mutex_destroy(&queueMoves4_mutex);
	  	case 4:
	    	free(nextMoves3);
	    	free(nextMoves2);
	    	pthread_mutex_destroy(&queueMoves3_mutex);
	    	pthread_mutex_destroy(&queueMoves2_mutex);
	  	case 2:
	    	free(nextMoves1);
	    	pthread_mutex_destroy(&queueMoves2_mutex);
	  	case 1:
	    	free(nextMoves0);
	    	pthread_mutex_destroy(&queueMoves0_mutex);
	  }
	  closeDb(db);
  }
	cleanup_magic_bitboards();
	if (res) return res;
	return numberOfGamesPlayed;
}

///<summary>
/// ECO classificator
///</summary>
void ecoClassify(struct Game * game, struct EcoLine ** ecoLine, int numberOfEcoLines) {
	size_t ecoLength = 0;
	int idx = -1;
	for (int i = 0; i < numberOfEcoLines; i++) {
		size_t newEcoLength = strlen(ecoLine[i]->sanMoves);
		if (strstr(game->sanMoves, ecoLine[i]->sanMoves) && newEcoLength > ecoLength) {
			ecoLength = newEcoLength;
			idx = i;
		}
	}
	if (idx >= 0) {
		if (ecoLine[idx]->tags[eECO][0] != '\0') {
			strncpy(game->tags[ECO], ecoLine[idx]->tags[eECO], MAX_TAG_VALUE_LEN);
		  //printf("%s\n", game->tags[ECO]);
		}
		if (ecoLine[idx]->tags[eOpening][0] != '\0') {
			strncpy(game->tags[Opening], ecoLine[idx]->tags[eOpening], MAX_TAG_VALUE_LEN);
		  //printf("%s\n", game->tags[Opening]);
		}
		if (ecoLine[idx]->tags[eVariation][0] != '\0') {
			strncpy(game->tags[Variation], ecoLine[idx]->tags[eVariation], MAX_TAG_VALUE_LEN);
		  //printf("%s\n", game->tags[Variation]);
    }
	}
}
