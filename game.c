#pragma warning(disable:4996)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h> //use -pthread when compiling and linking
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include "libchess.h"

struct Node {
  unsigned long hash;
  char sanMove[7];
  int score;
  struct Node * next;
};

struct Queue {
  struct Node * first;
  struct Node * last;
};

int dbinit(sqlite3 *, int, int, bool);
int dbsearch(sqlite3 *, unsigned long, enum Color, int, double, char **);
int dbbegin(sqlite3 *);
int dbcommit(sqlite3 *);
int dbsearchGame(sqlite3 *, unsigned long);
int dbupdate(sqlite3 *, unsigned long, char *, int);
int dbclose(sqlite3 *);

static unsigned long gameNumber = 0, numberOfGames, numberOfEcoLines = 0, skippedNumberOfGames = 0, numberOfDuplicateGames = 0;
static long gameStartPositions[MAX_NUMBER_OF_GAMES];
//static bool classify = false;
static int playGameResult[MAX_NUMBER_OF_THREADS];
static int updateDbResult;
static struct EcoLine * ecoLines[MAX_NUMBER_OF_ECO_LINES];
static sqlite3 * pDb = NULL;
static struct Queue * nmQueue = NULL;
static pthread_mutex_t gameNumber_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t nmQueue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t skippedNumberOfGames_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t numberOfDuplicateGames_mutex = PTHREAD_MUTEX_INITIALIZER;
struct PlayGameContext {
	char fileName[255];
  bool zobristHash;
  bool classify;
  int minElo;
  int maxEloDiff;
	int threadNumber;
};
struct UpdateDBContext {
    char fileName[255];
};

void enqueue(struct Queue * queue, unsigned long hash, char * sanMove, int score) {
  //printf("starting enqueue...\n");
  if (queue == NULL) return;
  struct Node * node = (struct Node *)malloc(sizeof(struct Node));
  node->hash = hash;
  strncpy(node->sanMove, sanMove, strlen(sanMove) + 1);
  node->score = score;
  node->next = NULL;
  if (queue->last == NULL)
    queue->first = queue->last = node;
  else { 
    queue->last->next = node;
    queue->last = node;
  }
  //printf("finished enqueue...\n");
}

struct Node * dequeue(struct Queue * queue) {
  //printf("starting dequeue...\n");
  if (queue == NULL) return NULL;
  if (queue->first == NULL) return NULL;
  struct Node * nextMove = queue->first;
  queue->first = queue->first->next;
  //printf("finished dequeue...\n");
  return nextMove; //must be freed
}

bool isEmptyLine(char * line) {
	size_t len = strlen(line);
	for (char c = 0; c < len; c++) {
		if (!isspace(line[c])) return false;
	}
	return true;
}

int normalizeMoves(char * moves) {
	char excludeChars[] = { '\n', '\v', '\t', ' ', '!', '?', '\r' };
	bool moreThanOne = false, skipCur = false, skipPar = false, dollar = false;
	int count = 0;
	unsigned short n = 0;
	char * s = strdup(moves);
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
	return 0;
}

int movesOnly(char *moves) {
	bool wroteSpace = false;
	unsigned short n = 0;
	char * s = strdup(moves);
	size_t len = strlen(s);
	for (int c = 1; c < len; c++) {
		if (s[c] == '.') continue;
		if (s[c] == ' ') {
			if (s[c - 1] == ' ') continue;
			else if (!wroteSpace) {
				moves[n++] = s[c];
				wroteSpace = true;
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
	moves[n] = '\0';
	return 0;
}

///<summary>
/// Count number of games from a file stream and index them by a game start position
///</summary>
unsigned long countGames(FILE * file, char * firstLine, long gameStartPositions[], unsigned long maxNumberOfGames) {
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
/*
	if (gameStartPositions[0] != -1L) {
		if (res = fseek(file, gameStartPositions[0], SEEK_SET)) {
			printf("countGames() error: seek returned %s\n", strerror(errno));
			return -1;
		}
	}
*/
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
		printf("gameTags() error: unexpected end of file\n");
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
		printf("eTags() error: unexpected end of file\n");
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

unsigned long getGameHash(char * sanMoves) {
  EVP_MD_CTX * mdCtx = NULL;
  EVP_MD * md5 = NULL;
  int err = 1, l;
  unsigned char * gHash = NULL;
  unsigned int len;
  unsigned long gHashH = 0UL, gHashL = 0UL;

  //printf("getGameHash(): calling EVP_MD_CTX_new()...\n");
  mdCtx = EVP_MD_CTX_new();
  if (mdCtx == NULL) goto error;
  //printf("getGameHash(): calling EVP_MD_fetch()...\n");
  md5 = EVP_MD_fetch(NULL, "MD5", NULL);
  if (md5 == NULL) goto error;
  //printf("getGameHash(): calling EVP_DigestInit_ex()...\n");
  if (!EVP_DigestInit_ex(mdCtx, md5, NULL)) goto error;
  //printf("getGameHash(): calling EVP_DigestUpdate()...\n");
  if (!EVP_DigestUpdate(mdCtx, sanMoves, strlen(sanMoves))) goto error;
  l = EVP_MD_get_size(md5);
  //printf("getGameHash(): calling OPENSSL_malloc()...\n");
  gHash = OPENSSL_malloc(l);
  if (gHash == NULL) goto error;
  //printf("getGameHash(): calling EVP_DigestFinal_ex()...\n");
  if (!EVP_DigestFinal_ex(mdCtx, gHash, &len)) goto error;
  if (l == 16) {
    //printf("getGameHash(): calling memcpy(&gHashL, gHash + 8, 8)...\n");
    memcpy(&gHashL, gHash + 8, 8);
    //printf("getGameHash(): calling memcpy(&gHashH, gHash, 8)...\n");
    memcpy(&gHashH, gHash, 8);
  } else printf("md5 hash size %d != 16\n", l);
  err = 0;
error:
  if (gHash) OPENSSL_free(gHash);
  if (md5) EVP_MD_free(md5);
  if (mdCtx) EVP_MD_CTX_free(mdCtx);
  if (err) {
    ERR_print_errors_fp(stdout);
	  return 0;
  }
  //printf("getGameHash(): returning hash gHashH ^ gHashL %lx...\n", gHashH ^ gHashL);
  return gHashH ^ gHashL;
}

///<summary>
/// plays a given pgn game
/// returns 0 on success, non-zero on error
///</summary>
int pgnGame(struct Game * game, bool generateZobristHash, unsigned long gNumber) {
	struct Move move;
	struct Board board;
	struct ZobristHash zh;//, zh2;
  struct ZobristHash * pzh;//, * pzh2;

	struct Fen fen;
	char * fenString;
  //printf("pgnGame: sanMoves %s\n", game->sanMoves);
	if (game->tags[FEN][0] == '\0') fenString = startPos;
	else fenString = game->tags[FEN];
	if (strtofen(&fen, fenString)) {
		printf("pgnGame() error: strtofen() failed; FEN %s\n", fenString);
		return 1;
	}
	if (fentoboard(&fen, &board)) {
		printf("pgnGame() error: fentoboard() failed; FEN %s\n", fen.fenString);
		return 1;
	}
	if (generateZobristHash) {
    pzh = &zh;
    //pzh2 = &zh2;
		zobristHash(pzh);
		//zobristHash(pzh2);
		getHash(pzh, &board);
    //printf("startPos hash = %lx, it should be %lx\n", pzh->hash, STARTPOS_HASH);
		//getHash(pzh2, &board);
    //if (pzh->hash != pzh2->hash) printf("pgnGame: pzh->hash %lx != pzh2->hash %lx\n", pzh->hash, pzh2->hash);
    //printf("startPos hash2 = %lx, it should be %lx\n", pzh2->hash, STARTPOS_HASH);
	} else pzh = NULL;
	
  //printf("pgnGame2: sanMoves %s\n", game->sanMoves);
	char * sanMoves = strdup(game->sanMoves);
	if (!sanMoves) {
		printf("pgnGame() error: strdup() returned %s\n", strerror(errno));
		return errno;
	}
  
  //printf("pgnGame: dupped sanMoves %s\n", sanMoves);
	char * saveptr;
	char * token = strtok_r(sanMoves, " ", &saveptr);
	while (token)
	{
    //printf("pgnGame: token %s\n", token);
		if (initMove(&move, &board, token)) {
			printf("pgnGame() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
			free(sanMoves);
			return 1;
		}

		makeMove(&move, pzh);
		//reconcile(&board);
    /*
		if (generateZobristHash) {
      getHash(pzh2, move.chessBoard);
      if (pzh2->hash != pzh->hash) {
  			printf("pgnGame() error: updated hash %lx is not equal board hash %lx after move %u%s%s (%s); FEN after the move %s\n", pzh->hash, pzh2->hash, move.chessBoard->fen->sideToMove == ColorWhite ? move.chessBoard->fen->moveNumber - 1 : move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? "... " : ". ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
	  		free(sanMoves);
        return 1;
      }
    }*/
    /*
		if (generateZobristHash) {
			if (!updateHash(&zh, &board, &move))
				board.hash = zh.hash;
			else {
				printf("pgnGame() error: updateHash() returned non-zero value\n");
				free(sanMoves);
				return 1;
			}
		}*/
		if (board.isStaleMate && strcmp(game->tags[Result], "1/2-1/2") != 0) {
			printf("Game %lu tag Result '%s' did not match the actual one '1/2-1/2': [Date \"%s\"], [White \"%s\"], [Black \"%s\"]\n", gNumber, game->tags[Result], game->tags[Date], game->tags[White], game->tags[Black]);
			strcpy(game->tags[Result], "1/2-1/2");
		}
		else if (board.isMate && board.movingPiece.color == ColorWhite && strcmp(game->tags[Result], "1-0") != 0) {
			printf("Game %lu tag Result '%s' did not match the actual one '1-0': [Date \"%s\"], [White \"%s\"], [Black \"%s\"]\n", gNumber, game->tags[Result], game->tags[Date], game->tags[White], game->tags[Black]);
			strcpy(game->tags[Result], "1-0");
		}
		else if (board.isMate && board.movingPiece.color == ColorBlack && strcmp(game->tags[Result], "0-1") != 0) {
			printf("Game %lu tag Result '%s' did not match the actual one '0-1': [Date \"%s\"], [White \"%s\"], [Black \"%s\"]\n", gNumber, game->tags[Result], game->tags[Date], game->tags[White], game->tags[Black]);
			strcpy(game->tags[Result], "0-1");
		}
   	token = strtok_r(NULL, " ", &saveptr);
 
    if (token) {
      int score = 0;
      if (strncmp(game->tags[Result], "1-0", 3) == 0) score = 1;
      else if (strncmp(game->tags[Result], "0-1", 3) == 0) score = -1;
      //queue nextMove
	  	pthread_mutex_lock(&nmQueue_mutex);
      enqueue(nmQueue, pzh->hash, token, score);
		  pthread_mutex_unlock(&nmQueue_mutex);
    }
  }
	free(sanMoves);
	return 0;
}

void * playGame(void * context) {
	struct Game game;
	char line[80];
	unsigned long gNumber, skipped = 0, duplicate = 0;
	struct PlayGameContext * ctx = (struct PlayGameContext *)context;
	FILE * file = fopen((char *)ctx->fileName, "r");
	if (!file) {
		printf("playGame() error: failed to open a file %s, %s\n", (char *)ctx->fileName, strerror(errno));
		playGameResult[ctx->threadNumber] = -1;
		return &(playGameResult[ctx->threadNumber]);
	}
 
	while (true) {
		pthread_mutex_lock(&gameNumber_mutex);
		gNumber = gameNumber++;
		pthread_mutex_unlock(&gameNumber_mutex);
		if (gNumber >= numberOfGames) break;

		fseek(file, gameStartPositions[gNumber], SEEK_SET);

		if (gTags(game.tags, file)) {
			printf("playGame() error: gTags() return non-zero result in game number %lu\n", gNumber);
			playGameResult[ctx->threadNumber] = -1;
			break;
		}

    //play the games that satisfies the minElo, maxEloDiff filter, as well as leave behind some erroneous setups for chess960, i.e. without FEN or SetUp tags
    if (ctx->minElo > atoi(game.tags[WhiteElo]) || ctx->minElo > atoi(game.tags[BlackElo]) || abs(atoi(game.tags[WhiteElo]) - atoi(game.tags[BlackElo])) > ctx->maxEloDiff || (atoi(game.tags[SetUp]) == 1 && (strlen(game.tags[FEN])) == 0 || ((strncmp(game.tags[Variant], variant[Chess960], strlen(variant[Chess960])) == 0) && (atoi(game.tags[SetUp]) != 1 || strlen(game.tags[FEN])) == 0))) {
      skipped++;
      continue;
    }

		//skip empty lines and tag lines            
		while (fgets(line, sizeof line, file)) {
			if (!isEmptyLine(line) && line[0] != '[') break;
		}
		game.sanMoves[0] = '\0';

		//Read game moves until first empty line
		strcat(game.sanMoves, line);
		while (fgets(line, sizeof line, file)) {
			if (isEmptyLine(line)) break;
			strcat(game.sanMoves, line);
		}
		//printf("playGame() %lu: raw game.sanMoves %s\n", gNumber, game.sanMoves);
		
		//update Result tag from the pgn moves, maybe it is not needed?
		char * endOfMoves = strstr(game.sanMoves, "1-0");
    int len = 3;
		if (!endOfMoves) {
			endOfMoves = strstr(game.sanMoves, "0-1");
			if (!endOfMoves) {
				endOfMoves = strstr(game.sanMoves, "1/2-1/2");
        len = 7;
				if (!endOfMoves) {
					endOfMoves = strstr(game.sanMoves, "*");
          len = 1;
				}
			}
		}
		//report discrepancies and strip the game result;
		if (endOfMoves) {
		  if (strncmp(game.tags[Result], endOfMoves, len) != 0) {
			  printf("Game %lu tag Result '%s' did not match the end of moves one '%s': [Date \"%s\"], [White \"%s\"], [Black \"%s\"]\n", gNumber, game.tags[Result], endOfMoves, game.tags[Date], game.tags[White], game.tags[Black]);
		  }      
      game.sanMoves[endOfMoves - game.sanMoves] = '\0';
    }
		//printf("playGame() %lu: game.sanMoves without result %s\n", gNumber, game.sanMoves);

		//strip comments, variations and NAGs
		normalizeMoves(game.sanMoves);
		//printf("playGame() %lu: normalized game.sanMoves %s\n", gNumber, game.sanMoves);

		//strip move numbers
		movesOnly(game.sanMoves);
		//printf("playGame() %lu: game.sanMoves only %s\n", gNumber, game.sanMoves);
    
    //calculate hash over sanMoves
    //the hash is used to ensure uniqueness of the games, so we don't take into account the same game more than once
    
    //printf("playGame(): calling getGameHash...\n");
    unsigned long gameHash = getGameHash(game.sanMoves);
    //printf("playGame(): calling getGameHash... done\n");
  
    if (!gameHash) {
		  printf("playGame() error: getGameHash(%s) returned 0\n", game.sanMoves);
		  playGameResult[ctx->threadNumber] = -1;
      break;
	  }
    
    //printf("playGame(): calling dbsearchGame(%lx)...\n", gameHash);
    int res = dbsearchGame(pDb, gameHash);
    //printf("playGame(): calling dbsearchGame(%lx)... done\n", gameHash);
    if (res < 0) {
      printf("playGame() error: dbsearchGame() returned negative result\n");
			playGameResult[ctx->threadNumber] = -1;
      break;
    } else if (res == 0) {
      duplicate++;
			//printf("Duplicate game: [Date \"%s\"], [White \"%s\"], [Black \"%s\"]\n", game.tags[Date], game.tags[White], game.tags[Black]);
			//printf("%lx %s\n", gameHash, game.sanMoves);
      continue;
    }

		struct Fen fen;
		char * fenString;
		if (game.tags[FEN][0] == '\0') fenString = startPos;
		else fenString = game.tags[FEN];
		if (strtofen(&fen, fenString)) {
			printf("playGame() error: strtofen() failed; FEN %s in game number %lu\n", fenString, gNumber);
			playGameResult[ctx->threadNumber] = -1;
			break;
		}
		if (ctx->classify && strcmp(game.tags[Variant], "chess 960") != 0 && fen.moveNumber == 1 && fen.sideToMove == ColorWhite) {
			ecoClassify(&game, ecoLines, numberOfEcoLines);
			//printf("ECO %s\nOpening %s\nVariation %s\n", game.tags[ECO], game.tags[Opening], game.tags[Variation]);
		}

    //printf("playGame: sanMoves %s\n", game.sanMoves);
		if (pgnGame(&game, ctx->zobristHash, gNumber)) {
			printf("playGame() error: pgnGame() returned non-zero result in game number %lu\n", gNumber);
			playGameResult[ctx->threadNumber] = -1;
			break;
		}
	}
  //free(game);
	fclose(file);
  pthread_mutex_lock(&skippedNumberOfGames_mutex);
  skippedNumberOfGames += skipped;
  pthread_mutex_unlock(&skippedNumberOfGames_mutex);
  pthread_mutex_lock(&numberOfDuplicateGames_mutex);
  numberOfDuplicateGames += duplicate;
  pthread_mutex_unlock(&numberOfDuplicateGames_mutex);
	return &(playGameResult[ctx->threadNumber]);
}

void * updatedb(void * context){
  struct UpdateDBContext * ctx = (struct UpdateDBContext *)context;

  //printf("updatedb(): calling dbbegin...\n");
	if (dbbegin(pDb)) {
		printf("updatedb() error: dbbegin(), returned non-zero result\n");
		updateDbResult = -1;
		return &updateDbResult;
	}
  //printf("updatedb(): calling dbbegin... done\n");
  
  //dequeue nextMove
  pthread_mutex_lock(&nmQueue_mutex);
  struct Node * nm = dequeue(nmQueue);
	pthread_mutex_unlock(&nmQueue_mutex);

  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 1000000;
  time_t t1 = time(NULL), t2 = t1;
  
  //wait for the next move queue to build but no longer than MAX_SEC_TO_WAIT_FOR_NM_QUEUE in sec
  while (nm == NULL && t2 - t1 <= MAX_SEC_TO_WAIT_FOR_NM_QUEUE) {
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&nmQueue_mutex);
    nm = dequeue(nmQueue);
	  pthread_mutex_unlock(&nmQueue_mutex);
    t2 = time(NULL);
  }

  //update next_moves db
  //printf("updatedb(): starting while loop...\n");
  while (nm != NULL) {
    dbupdate(pDb, nm->hash, nm->sanMove, nm->score);
    free(nm);
    pthread_mutex_lock(&nmQueue_mutex);
    nm = dequeue(nmQueue);
	  pthread_mutex_unlock(&nmQueue_mutex);
  }
  //printf("updatedb(): calling dbcommit...\n");
  dbcommit(pDb);
  //printf("updatedb(): calling dbcommit... done\n");
}

///<summary>
/// plays multiple pgn games from a given pgn file
/// second arguments is eco file name for ECO classification
/// returns the number of games or -1 in case of an error
///</summary>
unsigned long pgnGames(char * fileName, char * ecoFileName, char * nextMovesDB, bool zobristHash, int minElo, int maxEloDiff) {
	int err = 0;
  bool classify = false;
	FILE * file = fopen(fileName, "r");
	if (!file) {
		printf("countGames() error: failed to open a file %s, %s\n", fileName, strerror(errno));
		return 0;
	}
  
  err = sqlite3_open(nextMovesDB, &pDb);
  if (err != SQLITE_OK || pDb == NULL) {
    printf("pgnGames: sqlite3_open(%s) returned %d (%s)\n", nextMovesDB, err, sqlite3_errstr(err));
		return 0;
  }
  
  //printf("calling dbinit...\n");
	if (dbinit(pDb, SQLITE_CACHE_SIZE, SQLITE_MMAP_SIZE, SQLITE_JOURNAL_MODE)) {
		printf("pgnGames() error: dbinit(), returned non-zero result\n");
		return 0;
	}
  //printf("calling dbinit... done\n");
  
	if (ecoFileName) {
		FILE * ecoFile = fopen(ecoFileName, "r");
		if (!ecoFile) {
			printf("countGames() warning: failed to open a ECO file %s, %s\n", ecoFileName, strerror(errno));
		} else {
			long ecoLinesStartPositions[MAX_NUMBER_OF_ECO_LINES];
			numberOfEcoLines = countGames(ecoFile, "[ECO ", ecoLinesStartPositions, MAX_NUMBER_OF_ECO_LINES);
			if (numberOfEcoLines > MAX_NUMBER_OF_ECO_LINES) {
				printf("pgnGames() warning: number of lines in a eco file %s is %ld, which is greater than the maximum %d\n", ecoFileName, numberOfEcoLines, MAX_NUMBER_OF_ECO_LINES);
				numberOfEcoLines = MAX_NUMBER_OF_ECO_LINES;
			}

			char ecoLine[80];
			for (unsigned long i = 0; i < numberOfEcoLines; i++) {
				ecoLines[i] = malloc(sizeof(struct EcoLine));
				if (!ecoLines[i]) {
					printf("pgnGames() error: malloc failure\n");
					return 0;
				}
				if (eTags(ecoLines[i]->tags, ecoFile)) {
					printf("pgnGames() error: gTags() return non-zero result for eco file\n");
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
			classify = true;
			fclose(ecoFile);
		}
	}

	numberOfGames = countGames(file, "[Event ", gameStartPositions, MAX_NUMBER_OF_GAMES);
	fclose(file);

	if (numberOfGames > MAX_NUMBER_OF_GAMES) {
		printf("pgnGames() warning: number of games in a pgn file %s is %lu, which is greater than the maximum %d\n", fileName, numberOfGames, MAX_NUMBER_OF_GAMES);
		numberOfGames = MAX_NUMBER_OF_GAMES;
	}
	struct PlayGameContext * playGameCtx[MAX_NUMBER_OF_THREADS];
	struct UpdateDBContext * updateDbCtx;
  
  nmQueue = (struct Queue *)malloc(sizeof(struct Queue));
  if (!nmQueue) {
		printf("pgnGames() error: failed to allocate memory for nmQueue\n");
		return 0;    
  }
  nmQueue->first = nmQueue->last = NULL;
  
	//Multi-threading implementation, where each thread will process one game at a time
	//locking, updating and unlocking the current game number as well as queueing next_moves
  
  //A separate thread will dequeue next_moves and writes them to the sqlite3 db

	pthread_t thread_id[MAX_NUMBER_OF_THREADS], update_db_thread;
  pthread_attr_t attr;
	for (unsigned int t = 0; t < MAX_NUMBER_OF_THREADS; t++) {
		if (err = pthread_attr_init(&attr) != 0)
			printf("libchess pgnGames: pthread_attr_init(playGame) returned %d\n", err);
		else {
			playGameCtx[t] = malloc(sizeof(struct PlayGameContext));
			if (playGameCtx[t]) {
				strncpy(playGameCtx[t]->fileName, fileName, strlen(fileName) + 1);
        playGameCtx[t]->classify = classify;
        playGameCtx[t]->zobristHash = zobristHash;
        playGameCtx[t]->minElo = minElo;
        playGameCtx[t]->maxEloDiff = maxEloDiff;
				playGameCtx[t]->threadNumber = t;
				if (err = pthread_create(&(thread_id[t]), &attr, &playGame, (void *)(playGameCtx[t])) != 0)
					printf("libchess pgnGames: pthread_create(playGame) returned %d\n", err);
			}
			if (err = pthread_attr_destroy(&attr) != 0)
				printf("libchess pgnGames: pthread_attr_destroy(playGame) returned %d\n", err);
		}
	}
	if (err = pthread_attr_init(&attr) != 0)
		printf("libchess pgnGames: pthread_attr_init(updatedb) returned %d\n", err);
	else {
  	updateDbCtx = malloc(sizeof(struct UpdateDBContext));
		if (updateDbCtx) {
			strncpy(updateDbCtx->fileName, nextMovesDB, strlen(nextMovesDB) + 1);
			if (err = pthread_create(&update_db_thread, &attr, &updatedb, (void *)(updateDbCtx)) != 0)
				printf("libchess pgnGames: pthread_create(updatedb) returned %d\n", err);
		}
		if (err = pthread_attr_destroy(&attr) != 0)
			printf("libchess pgnGames: pthread_attr_destroy(updatedb) returned %d\n", err);
	}
	
	for (unsigned int t = 0; t < MAX_NUMBER_OF_THREADS; t++) {
		pthread_join(thread_id[t], NULL);
		free(playGameCtx[t]);
	}
	pthread_join(update_db_thread, NULL);
  free(updateDbCtx);

	for (int i = 0; i < numberOfEcoLines; i++) free(ecoLines[i]);
	printf("libchess pgnGames: skipped %ld\n", skippedNumberOfGames);
	printf("libchess pgnGames: duplicate %ld\n", numberOfDuplicateGames);
  free(nmQueue);
  dbclose(pDb);
  if (err) return 0;
	return numberOfGames;
}

///<summary>
/// ECO classificator
///</summary>
void ecoClassify(struct Game * game, struct EcoLine ** ecoLine, unsigned long numberOfEcoLines) {
	size_t ecoLength = 0;
	for (unsigned long i = 0; i < numberOfEcoLines; i++) {
		size_t newEcoLength = strlen(ecoLine[i]->sanMoves);
		if (strstr(game->sanMoves, ecoLine[i]->sanMoves) && newEcoLength > ecoLength)
		{
			if (ecoLine[i]->tags[eECO][0] != '\0') strncpy(game->tags[ECO], ecoLine[i]->tags[eECO], sizeof(char[MAX_TAG_VALUE_LEN]));
			if (ecoLine[i]->tags[eOpening][0] != '\0') strncpy(game->tags[Opening], ecoLine[i]->tags[eOpening], sizeof(char[MAX_TAG_VALUE_LEN]));
			if (ecoLine[i]->tags[eVariation][0] != '\0') strncpy(game->tags[Variation], ecoLine[i]->tags[eVariation], sizeof(char[MAX_TAG_VALUE_LEN]));
			ecoLength = newEcoLength;
		}
	}
}
