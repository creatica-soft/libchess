//This file is used to experiment with data and data loading to train chess AI models from PGN and CSV files
#pragma warning(disable:4996)
#include <omp.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <pthread.h> //use -pthread when compiling and linking
#include "uthash.h"
#include "magic_bitboards.h"
#include "libchess.h"
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
int boardLegalMoves(float * boards_legal_moves, int sample, int channels, struct Board * board);

struct GameHash {
    uint8_t hash[8];    // Key: 8-byte half-MD5 hash to filter identical games
    UT_hash_handle hh;  // Required for uthash
};

struct GameHash * games = NULL;

//BMPR stands for board, moves, promo, result
//It is used in ML data loading
struct NodeBMPR {
  struct BMPR * bmpr;
  struct NodeBMPR * next;
};
struct QueueBMPR {
  struct NodeBMPR * first;
  struct NodeBMPR * last;
  int len;
  int maxLen;
};
//this structure is used in getGame_detached() to start a detached thread
struct getGameCtx {
  char fileNames[256][256];
  int numberOfFiles;
  int minElo;
  int maxEloDiff;
  int minMoves;
  unsigned long steps; //for curriculum learning
  int numberOfChannels;
  int numberOfSamples;
  int bmprQueueLen;
  enum GameStage gameStage;
};

static struct QueueBMPR queueBMPR;

int enqueueBMPR(struct BMPR * bmpr) {
  pthread_mutex_lock(&queue_mutex); 
  if (!bmpr || queueBMPR.len > queueBMPR.maxLen) {
    pthread_mutex_unlock(&queue_mutex);
    return 1;
  }
  struct NodeBMPR * node = (struct NodeBMPR *)malloc(sizeof(struct NodeBMPR));
  node->bmpr = bmpr;
  node->next = NULL;
  if (queueBMPR.last == NULL)
    queueBMPR.first = queueBMPR.last = node;
  else { 
    queueBMPR.last->next = node;
    queueBMPR.last = node;
  }
  queueBMPR.len++;
  pthread_mutex_unlock(&queue_mutex);
  return 0;
}
struct BMPR * dequeueBMPR() {
  pthread_mutex_lock(&queue_mutex); 
  if (queueBMPR.first == NULL) {
    pthread_mutex_unlock(&queue_mutex);
    return NULL;
  }
  struct NodeBMPR * node = queueBMPR.first;
  struct BMPR * bmpr = node->bmpr;
  queueBMPR.first = queueBMPR.first->next;
  queueBMPR.len--;
  if (queueBMPR.len == 0) queueBMPR.last = NULL;
  free(node);
  pthread_mutex_unlock(&queue_mutex);
  return bmpr;
}

//enqueue if full and initilize new BMPR structure that contains chess position samples
struct BMPR * bmprNew(struct BMPR * bmpr, int channels, int numberOfSamples) {
  //printf("bmprNew() imput: bmpr at %p in thread %d\n", (void*)bmpr, omp_get_thread_num());
  if (bmpr) {
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 100000; //nanosec
    while (enqueueBMPR(bmpr)) {
      nanosleep(&delay, NULL);
      delay.tv_nsec *= 2;
    }
  }
  bmpr = (struct BMPR *)malloc(sizeof(struct BMPR));
  if (!bmpr) {
		printf("bmprNew(%d) error: malloc(bmpr) returned NULL\n", omp_get_thread_num());
		return NULL;    
  }
  //printf("Allocated bmpr at %p in thread %d\n", (void*)bmpr, omp_get_thread_num());
  bmpr->samples = numberOfSamples;
  bmpr->sample = 0;
  bmpr->channels = channels;
	bmpr->boards_legal_moves = (float *)calloc(bmpr->samples * bmpr->channels * 64, sizeof(float));
	if (!bmpr->boards_legal_moves) {
	  printf("bmprNew(%d) error: calloc() for boards_legal_moves returned NULL. %s\n", omp_get_thread_num(), strerror(errno));
	  return NULL;
	}
	bmpr->moves = (int *)calloc(bmpr->samples, sizeof(int));
	if (!bmpr->moves) {
	  printf("bmprNew(%d) error: calloc() for moves returned NULL. %s\n", omp_get_thread_num(), strerror(errno));
	  return NULL;
	}
	bmpr->result = (int *)calloc(bmpr->samples, sizeof(int));
	if (!bmpr->result) {
	  printf("bmprNew(%d) error: calloc() for result returned NULL. %s\n", omp_get_thread_num(), strerror(errno));
	  return NULL;
	}
	bmpr->stage = (int *)calloc(bmpr->samples, sizeof(int));
	if (!bmpr->stage) {
	  printf("bmprNew(%d) error: calloc() for stage returned NULL. %s\n", omp_get_thread_num(), strerror(errno));
	  return NULL;
	}
  return bmpr;
}

//return a game stage such as opening, middlegame or endgame enum GameStage
enum GameStage getStage(struct Board * board) {
  int numberOfPieces = __builtin_popcountl(board->occupations[PieceNameAny]);
  if (numberOfPieces >= 24) return OpeningGame;
  else if (numberOfPieces <= 12) return EndGame;
  else return MiddleGame;
}

//scalar value used in value head as an input
//not currently used
/*
float materialBalance(struct Board * board) {
  float white = 0, black = 0;
  for (enum PieceName pn = WhitePawn; pn <= WhiteQueen; pn++) {
    white += 2.0 * pieceValue[pn & 7] * __builtin_popcountl(board->occupations[pn]);
  }
  for (enum PieceName pn = BlackPawn; pn <= BlackQueen; pn++) {
    black += 2.0 * pieceValue[pn & 7] * __builtin_popcountl(board->occupations[pn]);
  }
  float balance = board->fen->sideToMove == ColorBlack ? (white - black) : (black - white);
  if (fabs(balance) < 0.001) balance = 0.0;
  //printf("materialBalance: white=%.2f, black=%.2f, balance=%.2f, sideToMove=%d\n", white, black, balance, board->fen->sideToMove);
  return balance;
}
*/

///<summary>
/// Plays a given game using Game struct
/// Returns struct BMPR *
///</summary>
int playGameAI(struct Game * game, struct BMPR ** bmpr, enum GameStage gameStage, unsigned long * step, const unsigned long steps) {
  int gameResult = 0;
	struct Move move;
	struct Board board;
	struct Fen fen;
	char * fenString;
  bool wrong_result = false;
  
	if (strcmp(game->tags[Result], "1/2-1/2") == 0) gameResult = 0;
	else if (strcmp(game->tags[Result], "1-0") == 0) gameResult = 1;
	else if (strcmp(game->tags[Result], "0-1") == 0) gameResult = -1;
	else if (strcmp(game->tags[Result], "*") == 0) gameResult = 0;
	else printf("playGameAI(%d) error: unknown game result (or missing Result tag), assuming a draw\n", omp_get_thread_num());
	if (game->tags[FEN][0] == '\0') fenString = startPos;
	else fenString = game->tags[FEN];
	//printf("playGameAI(): fen string %s\n", fenString);
	if (strtofen(&fen, fenString)) {
		printf("playGameAI(%d) error: strtofen() failed; FEN %s\n", omp_get_thread_num(), fenString);
		return -1;
	}
	if (fentoboard(&fen, &board)) {
		printf("playGameAI(%d) error: fentoboard() failed; FEN %s\n", omp_get_thread_num(), fen.fenString);
		return -2;
	}
  
	char * sanMoves = NULL;
	//printf("playGameAI(): calling strdup() with game->sanMoves %s...\n", game->sanMoves);
	//printf("playGameAI(%d) sanMoves: \"%s\"\n", 0, game->sanMoves);
	sanMoves = strndup(game->sanMoves, strlen(game->sanMoves));
	if (!sanMoves) {
		printf("playGameAI(%d) error: strndup() returned NULL: %s. sanMoves %s\n", omp_get_thread_num(), strerror(errno), game->sanMoves);
		return -7;
	}
	char * saveptr;
	char * token = strtok_r(sanMoves, " ", &saveptr);
	while (token) {
	  enum GameStage stage = getStage(&board);
	  //printf("gameStage %s\n", gameStages[stage]);
	  if (gameStage != FullGame) {
  	  if (stage != gameStage) { //skip moves until we get to the required stage
    		if (initMove(&move, &board, token)) {
    			printf("playGameAI(%d) error: invalid move %u%s%s (%s); FEN %s\n", omp_get_thread_num(), move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
    			if (sanMoves) free(sanMoves);
    			return -2;
    		}
    		makeMove(&move);    
    		token = strtok_r(NULL, " ", &saveptr);
  	    continue;
  	  }
	  } /*else {
	    //this blocks attempts to balance the number of positions for each game stage in a batch
  	  //so that each AI model head (opening, middlegame, endgame) receives approximately the same
  	  //number of samples
  	  //curriculum learning: start with simple positions of 5 legal moves, gradually increase to 20 legal moves
    	
    	float weights[] = {0.45, 0.65, 1.0}; // Opening, middlegame, endgame
    	float r = rand() / (float)RAND_MAX;
    	float r2;
    	#pragma omp critical
    	r2 = *step / (float)steps;
      if (r > weights[stage] || board.numberOfMoves > 5 + 15 * r2) {
    		if (initMove(&move, &board, token)) {
    			printf("playGameAI(%d) error: invalid move %u%s%s (%s); FEN %s\n", omp_get_thread_num(), move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
    			if (sanMoves) free(sanMoves);
    			return -2;
    		}
    		makeMove(&move);    
    		token = strtok_r(NULL, " ", &saveptr);
  	    continue;        
      }
      #pragma omp critical
      (*step)++;
	  }*/
	  (*bmpr)->stage[(*bmpr)->sample] = stage;
	  //populate channels used as input for AI model
    if (boardLegalMoves((*bmpr)->boards_legal_moves, (*bmpr)->sample, (*bmpr)->channels, &board)) {
      printf("boardLegalMoves() returned non-zero code\n");
      return -1;
    }
		if (initMove(&move, &board, token)) {
			printf("playGameAI(%d) error: invalid move %u%s%s (%s); FEN %s\n", omp_get_thread_num(), move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
			if (sanMoves) free(sanMoves);
			return -2;
		}
    //calculate legal move index (move prediction class) - used as target in policy loss calculation
    (*bmpr)->moves[(*bmpr)->sample] = move.sourceSquare.name * 64 + move.destinationSquare.name;
    //calculate promo index (promo prediction class) - used as target in promo loss calculation
    //I think underpromotions are extremely rare, so I think it would be impossible to learn them
    //We just always use default promotion to queen and be done with it
    /* 
    if (board.promoPiece != PieceNameNone)
      // promo classes: knight is 0, bishop is 1, rook is 2, queen is 3 
      (*bmpr)->promos[(*bmpr)->sample] = board.promoPiece - 1; 
      else (*bmpr)->promos[(*bmpr)->sample] = -1; //it should be masked to avoid negative numbers in loss
    */
		makeMove(&move);
		
		if (board.isStaleMate && strcmp(game->tags[Result], "1/2-1/2") != 0) {
			printf("playGameAI(%d): game tag Result '%s' did not match the actual one '1/2-1/2' - corrected\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", omp_get_thread_num(), game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
			strcpy(game->tags[Result], "1/2-1/2");
			gameResult = 0;
			wrong_result = true;
		} else if (board.isMate && board.movingPiece.color == ColorWhite && strcmp(game->tags[Result], "1-0") != 0) {
			printf("playGameAI(%d): game tag Result '%s' did not match the actual one '1-0' - corrected\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", omp_get_thread_num(), game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
			strcpy(game->tags[Result], "1-0");
			gameResult = 1;
			wrong_result = true;
		} else if (board.isMate && board.movingPiece.color == ColorBlack && strcmp(game->tags[Result], "0-1") != 0) {
			printf("playGameAI(%d): game tag Result '%s' did not match the actual one '0-1' - corrected\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", omp_get_thread_num(), game->tags[Result], game->tags[Event], game->tags[Site], game->tags[Date], game->tags[Round], game->tags[White], game->tags[Black]);
			strcpy(game->tags[Result], "0-1");
			gameResult = -1;
			wrong_result = true;
		}
		//we attempt to correct wrong result by using the end of move result in stripGameResult()
		//but it also may be wrong
		//result is from a point of view of the side to move - which I think confuses the model during training!
		//so let's provide constant result regardless of the side to move!
		//result class is used as a target in value loss calculation
		//if (board.fen->sideToMove == ColorBlack) //move is made, so sideToMove if White has already changed to Black
    (*bmpr)->result[(*bmpr)->sample] = gameResult + 1; //(0 (white lost/black won), 1 (draw), 2 (white won/black lost))
    //else 
    //(*bmpr)->result[(*bmpr)->sample] = -gameResult + 1; //from black perspective (0 (loss), 1 (draw), 2 (win))
    //we attempt to correct the wrong game result by rewinding backwards by the number of plies in a game
    //if the game started in previous batch, then those positions won't be updated, of course
    if (wrong_result) {
      for (int i = 1; i < board.plyNumber; i++) {
        if ((*bmpr)->sample - i < 0) break;
        (*bmpr)->result[(*bmpr)->sample - i] = (*bmpr)->result[(*bmpr)->sample];
      }
    }
    //printf("playGameAI(%d): sample %d, result %d, gameResult %d, sideToMove %d\n", omp_get_thread_num(), (*bmpr)->sample, (*bmpr)->result[(*bmpr)->sample], gameResult, board.fen->sideToMove);
    //material balance is used as scalar input in boath policy and value heads 
    //material balance seems to be irrelevant for a single position, which could just be in the middle
    //of the exchange, so let's drop it
    //(*bmpr)->material_balance[(*bmpr)->sample] = materialBalance(&board);
    //printf("playGameAI(%d): sample %d, material_balance %.2f\n", omp_get_thread_num(), (*bmpr)->sample, (*bmpr)->material_balance[(*bmpr)->sample]);
    //side to move is used as scalar input in both policy and value head
    //it is not useful, let's skip it
    //(*bmpr)->side_to_move[(*bmpr)->sample] = board.fen->sideToMove == ColorWhite ? 1.0 : -1.0;
    if ((*bmpr)->sample == (*bmpr)->samples - 1) {
      *bmpr = bmprNew(*bmpr, (*bmpr)->channels, (*bmpr)->samples);
  	} else {
  	 (*bmpr)->sample++; 
  	}
		//reconcile(&board);
		token = strtok_r(NULL, " ", &saveptr);
	}
	if (sanMoves) free(sanMoves);
	return 0;
}

void free_bmpr(struct BMPR * bmpr) {
  if (!bmpr) return;
  if (bmpr->boards_legal_moves) {
    free(bmpr->boards_legal_moves);
  }
  if (bmpr->moves) {
    free(bmpr->moves);
  }
  if (bmpr->result) {
    free(bmpr->result);
  }
  if (bmpr->stage) {
    free(bmpr->stage);
  }
  //printf("Freeing bmpr at %p in thread %d\n", (void*)bmpr, omp_get_thread_num());
  free(bmpr);
  bmpr = NULL;
}
//this function will use OMP library to run multiple tasks in parallel
//each task opens its own PGN file and plays its games filling BMPR queue
//the queue is consumed by ML data loading (see train_chess_cnn2.cpp)
//you might want to increase the number of threads in #pragma omp parallel num_threads(1)
void * getGame(void * context) {
  //printf("getGame() thread id: %d\n", omp_get_thread_num());
  unsigned long numberOfGames, skipped, duplicate, numberOfPlies, step; 
  pthread_mutex_lock(&queue_mutex);
  queueBMPR.len = 0;
  queueBMPR.maxLen = ((struct getGameCtx *)context)->bmprQueueLen;
  queueBMPR.first = NULL;
  queueBMPR.last = NULL;
  games = NULL;
  numberOfGames = 0;
  skipped = 0;
  duplicate = 0;
  numberOfPlies = 0;
  step = 0;
  pthread_mutex_unlock(&queue_mutex);
  omp_set_dynamic(1); // when set to 1, num_threads(x) will allow any number betweem 0 and x
  #pragma omp parallel shared(queueBMPR,games,numberOfGames,skipped,duplicate,numberOfPlies,step) num_threads(1)
  {
//commented out block is used for testing to empty BMPR queue
/*
    #pragma omp master
    {
      int sleepCount = 0;
      unsigned long freeCount = 0;
      struct timespec delay;
      delay.tv_sec = 1;
      delay.tv_nsec = 0;
      struct BMPR * bmpr = NULL;
      
      while (true) {
        //printf("%d queueBMPR len %d\n", omp_get_thread_num(), queueBMPR.len);
        #pragma omp critical 
        bmpr = dequeueBMPR();
        if (bmpr) {
          free_bmpr(bmpr);
          freeCount++;
          sleepCount = 0;
          //printf("%d queueBMPR len %d\n", omp_get_thread_num(), queueBMPR.len);          
        }
        else {
          if (sleepCount < 10) {
            printf("%d queueBMPR is empty, sleeping for 1 sec...freeCount %lu\n", omp_get_thread_num(), freeCount);
            nanosleep(&delay, NULL);
            sleepCount++;
            continue;
          } else {
            printf("%d queueBMPR is empty, exiting... freeCount %lu\n", omp_get_thread_num(), freeCount);
            break;          
          } 
        }
      }
    }
*/
    int i;
    struct getGameCtx * ctx;
    #pragma omp single private(i, ctx) firstprivate(context)
    {
      //printf("omp single thread id: %d\n", omp_get_thread_num());
      #pragma omp taskgroup
      {
        ctx = (struct getGameCtx *)context;
        for (i = 0; i < ctx->numberOfFiles; i++) {
          char filename[256];
          strncpy(filename, ctx->fileNames[i], 256);
          const int minElo = ctx->minElo;
          const int maxEloDiff = ctx->maxEloDiff;
          const int minMoves = ctx->minMoves;
          const unsigned long steps = ctx->steps;
          const int numberOfChannels = ctx->numberOfChannels;
          const int numberOfSamples = ctx->numberOfSamples;
          const enum GameStage gameStage = ctx->gameStage;
      	  #pragma omp task firstprivate(filename, minElo, maxEloDiff, minMoves, steps, numberOfChannels, numberOfSamples, gameStage) shared(queueBMPR, games, numberOfGames, skipped, duplicate, numberOfPlies, step)
      	  {
        	  int res = 0;
        	  printf("getGame(%d) opening file %s...\n", omp_get_thread_num(), filename);
        		FILE * file = fopen(filename, "r");
        		if (!file) {
        			printf("getGame(%d) error: failed to open a file %s, %s\n", omp_get_thread_num(), filename, strerror(errno));
        			res = 1;
        		}
        		struct BMPR * bmpr2 = bmprNew(NULL, numberOfChannels, numberOfSamples);
         	  if (!bmpr2) {
          	  printf("getGame(%d) error: bmprNew() returned NULL\n", omp_get_thread_num());
          	  res = 1;   	    
        	  }     		
        		while (true) {
        		  if (res == 1) break;
              
              struct Game game;
              if ((res = initGame(&game, file))) { //eof res = 1
                printf("getGame(%d): initGame() returned EOF for %s\n", omp_get_thread_num(), filename);  
                break;
              }
              #pragma omp critical
              numberOfGames++;
            	if (minElo > atoi(game.tags[WhiteElo]) || minElo > atoi(game.tags[BlackElo]) || maxEloDiff < abs(atoi(game.tags[WhiteElo]) - atoi(game.tags[BlackElo])) || minMoves * 2 > game.numberOfPlies - 1) {
                #pragma omp critical
            	  skipped++;
            		continue;
              }
              unsigned long hash = md5(game.sanMoves);			  	   
              struct GameHash * entry = NULL;
              #pragma omp critical
              HASH_FIND(hh, games, (void *)(&hash), 8, entry);
              if (entry) {
                #pragma omp critical
                duplicate++;
                res = 2;
              }
              else { 
                entry = malloc(sizeof(struct GameHash));
                memcpy(entry->hash, (void *)(&hash), 8);
                #pragma omp critical
                HASH_ADD(hh, games, hash, 8, entry);
              }
              if (res == 2) { 
                res = 0; 
                continue; 
              }
              #pragma omp critical
              numberOfPlies += game.numberOfPlies;
              res = playGameAI(&game, &bmpr2, gameStage, &step, steps);
              if (res < 0) break;
            } //end of while (true)
            if (bmpr2) {
              if (bmpr2->sample == 0) {
                free_bmpr(bmpr2);
              } else {
                bmpr2->samples = bmpr2->sample;
                struct timespec delay;
                delay.tv_sec = 0;
                delay.tv_nsec = 1000000; //nanosec
                while (enqueueBMPR(bmpr2)) {
                  nanosleep(&delay, NULL);
                  delay.tv_nsec *= 2;
                }
              }
            } //end of if (bmpr2)
          } //end of pragma omp task
        } //end of for loop over files
        free(ctx);
      } //end of pragma omp taskgroup
    } //end of pragma omp single
  } //end of pragma omp parallel
  struct GameHash * entry = NULL, * tmp = NULL;
  HASH_ITER(hh, games, entry, tmp) {
    HASH_DEL(games, entry);
    free(entry);
  }
  cleanup_magic_bitboards();
  games = NULL;
  printf("Total games %lu: played %lu, skipped %lu, duplicate %lu. Number of plies in played games: %lu, step %lu\n", numberOfGames, numberOfGames - skipped - duplicate, skipped, duplicate, numberOfPlies, step);
	return NULL;
}

//this is almost the same as getGame() but for csv files instead of PGN
//CSV file has three fields, separated by comma: FEN,UCI_move,Result (-1, 0, 1)
//you might want to increase the number of threads in #pragma omp parallel num_threads(1)
void * getGameCsv(void * context) {
  //printf("getGame() thread id: %d\n", omp_get_thread_num());
  unsigned long numberOfPlies;
  pthread_mutex_lock(&queue_mutex);
  numberOfPlies = 0;
  queueBMPR.len = 0;
  queueBMPR.maxLen = ((struct getGameCtx *)context)->bmprQueueLen;
  queueBMPR.first = NULL;
  queueBMPR.last = NULL;
  pthread_mutex_unlock(&queue_mutex);
  omp_set_dynamic(1); // when set to 1, num_threads(x) will allow any number betweem 0 and x
  #pragma omp parallel shared(queueBMPR) num_threads(1)
  {
//commented out block is used for testing to empty BMPR queue
/*
    #pragma omp master
    {
      int sleepCount = 0;
      unsigned long freeCount = 0;
      struct timespec delay;
      delay.tv_sec = 1;
      delay.tv_nsec = 0;
      struct BMPR * bmpr2 = NULL;
      
      while (true) {
        //printf("%d queueBMPR len %d\n", omp_get_thread_num(), queueBMPR.len);
        #pragma omp critical 
        bmpr2 = dequeueBMPR();
        if (bmpr2) {
          free_bmpr(bmpr2);
          freeCount++;
          sleepCount = 0;
          //printf("%d queueBMPR len %d\n", omp_get_thread_num(), queueBMPR.len);          
        }
        else {
          if (sleepCount < 10) {
            printf("%d queueBMPR is empty, sleeping for 1 sec...freeCount %lu\n", omp_get_thread_num(), freeCount);
            nanosleep(&delay, NULL);
            sleepCount++;
            continue;
          } else {
            printf("%d queueBMPR is empty, exiting... freeCount %lu\n", omp_get_thread_num(), freeCount);
            break;          
          } 
        }
      }
    }
*/
    int i;
    struct getGameCtx * ctx;
    #pragma omp single private(i, ctx) firstprivate(context)
    {
      //printf("omp single thread id: %d\n", omp_get_thread_num());
      #pragma omp taskgroup
      {
        ctx = (struct getGameCtx *)context;
        for (i = 0; i < ctx->numberOfFiles; i++) {
          char filename[256];
          strncpy(filename, ctx->fileNames[i], 256);
          int numberOfChannels = ctx->numberOfChannels;
          int numberOfSamples = ctx->numberOfSamples;
          enum GameStage gameStage = ctx->gameStage;
      	  #pragma omp task firstprivate(filename, numberOfChannels, numberOfSamples, gameStage) shared(queueBMPR, numberOfPlies)
      	  {
        	  int res = 0;
        	  printf("getGameCsv(%d) opening file %s...\n", omp_get_thread_num(), filename);
        		FILE * file = fopen(filename, "r");
        		if (!file) {
        			printf("getGameCsv(%d) error: failed to open a file %s, %s\n", omp_get_thread_num(), filename, strerror(errno));
        			res = 1;
        		}
        		struct BMPR * bmpr = bmprNew(NULL, numberOfChannels, numberOfSamples);
         	  if (!bmpr) {
          	  printf("getGameCsv(%d) error: bmprNew() returned NULL\n", omp_get_thread_num());
          	  res = 1;   	    
        	  }
        	  char line[128];
            int gameResult = 0;
          	struct Move move;
          	struct Board board;
          	struct Fen fen;
          	char * fenString;
          	char * csv = NULL;
            while (fgets(line, sizeof line, file)) {
          		if (res == 1) break;
              if (feof(file)) {
            		printf("getGameCsv(%d): end of file %s\n", omp_get_thread_num(), filename);
            		break;
              }
            	csv = strndup(line, strlen(line));
            	if (!csv) {
            		printf("getGameCsv(%d) error: strndup() returned NULL: %s. csv line %s\n", omp_get_thread_num(), strerror(errno), line);
            		break;
            	}

            	char * saveptr;
            	char * token = strtok_r(csv, ",", &saveptr);
            	if (!token) {
            		printf("getGameCsv(%d): mulformed CSV line %s in file %s\n", omp_get_thread_num(), line, filename);
            	  break;
            	}
          	  if (strtofen(&fen, token)) {
            		printf("getGameCsv(%d) error: strtofen() failed; FEN %s\n", omp_get_thread_num(), fenString);
            		break;
            	} 
            	if (fentoboard(&fen, &board)) {
            		printf("getGameCsv(%d) error: fentoboard() failed; FEN %s\n", omp_get_thread_num(), fen.fenString);
            		break;
            	}
          	  //populate 90 channels used as input to policy head
          	  //printf("calling boardLegalMoves()...\n");
              if (boardLegalMoves(bmpr->boards_legal_moves, bmpr->sample, bmpr->channels, &board)) {
                printf("getGameCsv() error: boardLegalMoves() returned non-zero code\n");
                break;
              }
            	token = strtok_r(NULL, ",", &saveptr);
            	if (!token) {
            		printf("getGameCsv(%d): mulformed CSV line %s in file %s\n", omp_get_thread_num(), line, filename);
            	  break;
            	}
          	  //printf("calling boardLegalMoves()...done\n");
          		if (initMove(&move, &board, token)) {
          			printf("getGameCsv(%d) error: invalid move %u%s%s (%s); FEN %s\n", omp_get_thread_num(), move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
          			break;
          		}
              //calculate legal move index (move prediction class) - used as target in policy loss calculation
              bmpr->moves[bmpr->sample] = move.sourceSquare.name * 64 + move.destinationSquare.name;
              //calculate promo index (promo prediction class) - used as target in promo loss calculation
              /*if (board.promoPiece != PieceNameNone)
              // promo classes: knight is 0, bishop is 1, rook is 2, queen is 3 
              bmpr->promos[bmpr->sample] = board.promoPiece - 1; 
              else bmpr->promos[bmpr->sample] = -1; //it should be masked to avoid negative numbers in loss*/
            	token = strtok_r(NULL, ",", &saveptr);
            	if (!token) {
            		printf("getGameCsv(%d): mulformed CSV line %s in file %s\n", omp_get_thread_num(), line, filename);
            	  break;
            	}
            	if (csv) {
            	  free(csv);
            	  csv = NULL;
            	}
          		//result is from a point of view of the side to move
          		//result class is used as a target in value loss calculation
          		gameResult = atoi(token);
          		if (board.fen->sideToMove == ColorBlack) //move is made, so sideToMove has already changed
                bmpr->result[bmpr->sample] = gameResult + 1;
              else 
                bmpr->result[bmpr->sample] = -gameResult + 1;
              //material balance is used as scalar input in boath policy and value heads
              /*
              bmpr->material_balance[bmpr->sample] = materialBalance(&board);
              //side to move is used as scalar input in both policy and value head
              bmpr->side_to_move[bmpr->sample] = board.fen->sideToMove == ColorWhite ? 1.0 : -1.0;*/
              if (bmpr->sample == bmpr->samples - 1)
                bmpr = bmprNew(bmpr, bmpr->channels, bmpr->samples);
            	else bmpr->sample++;

              #pragma omp critical
              numberOfPlies++;
            } //end of while (true)
            if (bmpr) {
              if (bmpr->sample == 0) {
                free_bmpr(bmpr);
              } else {
                bmpr->samples = bmpr->sample;
                struct timespec delay;
                delay.tv_sec = 0;
                delay.tv_nsec = 1000000; //nanosec
                while (enqueueBMPR(bmpr)) {
                  nanosleep(&delay, NULL);
                  delay.tv_nsec *= 2;
                }
              }
            } //end of if (bmpr)
          } //end of pragma omp task
        } //end of for loop over files
        free(ctx);
      } //end of pragma omp taskgroup
    } //end of pragma omp single
  } //end of pragma omp parallel
  cleanup_magic_bitboards();
  printf("getGameCsv(): Number of plies in played games: %lu\n", numberOfPlies);
	return NULL;
}

//this function starts a detached thread that runs getGame() or getGameCSV() depending on file extension
void getGame_detached(char ** fileNames, const int numberOfFiles, const int minElo, const int maxEloDiff, const int minMoves, const int numberOfChannels, const int numberOfSamples, const int bmprQueueLen, const enum GameStage gameStage, const unsigned long steps) {
  int res;
  struct getGameCtx * ctx = (struct getGameCtx *)malloc(sizeof(struct getGameCtx));
  if (numberOfFiles > 256) {
    printf("numberOfFiles is too big, 256 is the max\n");
    exit(1);
  }
  for (int i = 0; i < numberOfFiles; i++)
    strncpy(ctx->fileNames[i], fileNames[i], 256);
  ctx->numberOfFiles = numberOfFiles;
  ctx->minElo = minElo;
  ctx->maxEloDiff = maxEloDiff;
  ctx->minMoves = minMoves;
  ctx->steps = steps;
  ctx->numberOfChannels = numberOfChannels;
  ctx->numberOfSamples = numberOfSamples;
  ctx->bmprQueueLen = bmprQueueLen;
  ctx->gameStage = gameStage;
  pthread_t thread;
	pthread_attr_t attr;
	if ((res = pthread_attr_init(&attr)) != 0) {
  	printf("getGame_detached() error: pthread_attr_init() returned %d\n", res);
  	exit(1);
  }
  init_magic_bitboards();
  if (strstr(fileNames[0], ".csv")) {
    if (pthread_create(&thread, &attr, &getGameCsv, (void *)ctx) == 0) {
        if ((res = pthread_attr_destroy(&attr)) != 0) {
      	  printf("getGame_detached() error: pthread_attr_destroy() returned %d\n", res);
      	  exit(1);
      	}
        pthread_detach(thread);  // Detach to run independently
        //pthread_join(thread, NULL);
    } else {
      free(ctx);
      printf("getGame_detached() error: pthread_create() returned NULL\n");
      exit(1);
    }    
  } else {
    if (pthread_create(&thread, &attr, &getGame, (void *)ctx) == 0) {
        if ((res = pthread_attr_destroy(&attr)) != 0) {
      	  printf("getGame_detached() error: pthread_attr_destroy() returned %d\n", res);
      	  exit(1);
      	}
        pthread_detach(thread);  // Detach to run independently
        //pthread_join(thread, NULL);
    } else {
      free(ctx);
      printf("getGame_detached() error: pthread_create() returned NULL\n");
      exit(1);
    }
  }
}
