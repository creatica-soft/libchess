// compile with c++ -std=c++17 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -Wl,-lchess -L /Users/ap/libchess -o tournament tournament.cpp

#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <unordered_map>
#include "libchess.h"

#define WHITE_PATH "/Users/ap/libchess/chess_mcts_smp"
#define BLACK_PATH "/Users/ap/libchess/stockfish-macos-m1-apple-silicon"
#define SYZYGY_PATH "/Users/ap/syzygy"
#define PGN_FILE "creatica-stockfish"
#define ELO_WHITE 1000
#define ELO_BLACK 1320
#define K_FACTOR 32 // 40 for new players (under 30 games), 20 for established players under 2400, 10 for masters (2400+) 
#define MOVETIME 1000
#define DEPTH 0
#define HASH 1024
#define THREADS 8
#define NUMBER_OF_GAMES 10

#ifdef __cplusplus
extern "C" {
#endif

//Globals
double e_white, e_black, score_white, score_black;
float elo_white, elo_black;
struct Engine * white = NULL, * black = NULL;
struct Board board;
struct Fen fen;
struct ZobristHash zh;
struct Move move;
struct Evaluation evaluation;
struct Evaluation * evaluations[1] = { NULL };
char sanMoves[4096] = "";
FILE * file = NULL;
char filename[255];

struct PairHash {
    std::size_t operator()(const std::pair<uint64_t, uint64_t>& p) const {
        // Combine the two 64-bit hashes using XOR
        // You can also use other mixing functions for better distribution
        //return std::hash<uint64_t>{}(p.first) ^ std::hash<uint64_t>{}(p.second);
        return p.first ^ p.second;
    }
};
  
void playGame(int n) {
  fprintf(stdout, "game %d, score %.1f : %.1f\n", n, score_white, score_black);
  newGame(white);
  newGame(black);
  strtofen(&fen, startPos);
  fentoboard(&fen, &board);
  zobristHash(&zh);
  board.zh = &zh;
  sanMoves[0] = 0;
  std::unordered_map<std::pair<uint64_t, uint64_t>, int, PairHash> position_history;
  std::pair<uint64_t, uint64_t> pos_key;
  bool repetition3x = false;
  while (!board.isMate && !board.isStaleMate && board.fen->halfmoveClock != 50 && !(__builtin_popcountl(board.occupations[PieceNameAny]) <= 5 && evaluations[0]->scorecp == 0) && !repetition3x) {
    strncpy(white->position, board.fen->fenString, MAX_FEN_STRING_LEN);
    if (!position(white)) {
      fprintf(stderr, "error: position(white) returned false, fen %s\n", white->position);
      exit(2);
    }
    if (go(white, evaluations)) {
      fprintf(stderr, "error: go(white, evaluations) returned non-zero code, fen %s\n", white->position);
      exit(2);
    }
  	if (initMove(&move, &board, evaluations[0]->bestmove)) {
    	fprintf(stderr, "error: invalid move %u%s%s (%s); FEN %s, bestmove %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString, evaluations[0]->bestmove);
    	exit(1);
    }
    strcat(sanMoves, move.sanMove);
    strcat(sanMoves, " ");
    makeMove(&move);
    fprintf(stdout, "game %d, score %.1f : %.1f, white score cp %d\n", n, score_white, score_black, evaluations[0]->scorecp);
    writeDebug(&board, false);
    if (updateHash(&board, &move)) {
        fprintf(stderr, "playGame() error: updateHash() returned non-zero value\n");
        exit(-1);
    }
    pos_key = std::make_pair(board.zh->hash, board.zh->hash2);
    if (position_history[pos_key] >= 2) { // 3rd occurrence
        repetition3x = true;
        break;
    }
    position_history[pos_key]++;
    if (board.isMate || board.isStaleMate || board.fen->halfmoveClock == 50 && !(__builtin_popcountl(board.occupations[PieceNameAny]) <= 5 && evaluations[0]->scorecp == 0) || repetition3x) {
      //fprintf(stderr, "mate, stalemate or halfmoveClock = 50\n");
      break;
    }
    strncpy(black->position, board.fen->fenString, MAX_FEN_STRING_LEN);
    if (!position(black)) {
      fprintf(stderr, "error: position(black) returned false, fen %s\n", black->position);
      exit(2);
    }
    if (go(black, evaluations)) {
      fprintf(stderr, "error: go(black, evaluations) returned non-zero code, fen %s\n", black->position);
      exit(2);
    }
  	if (initMove(&move, &board, evaluations[0]->bestmove)) {
    	fprintf(stderr, "error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
    	exit(1);
    }
    strcat(sanMoves, move.sanMove);
    strcat(sanMoves, " ");
    makeMove(&move);
    fprintf(stdout, "game %d, score %.1f : %.1f, white score cp %d\n", n, score_white, score_black, -evaluations[0]->scorecp);
    writeDebug(&board, false);
    if (updateHash(&board, &move)) {
        fprintf(stderr, "playGame() error: updateHash() returned non-zero value\n");
        exit(-1);
    }
    pos_key = std::make_pair(board.zh->hash, board.zh->hash2);
    if (position_history[pos_key] >= 2) { // 3rd occurrence
        repetition3x = true;
        break;
    }
    position_history[pos_key]++;
  }
  position_history.clear();
  
  char result[8];
  if (board.isMate) {
    if (board.fen->sideToMove == ColorWhite) {
      strcpy(result, "0-1");
      score_black += 1;
    }
    else {
      strcpy(result, "1-0");        
      score_white += 1.0;
    }
  } 
  else if (board.isStaleMate || board.fen->halfmoveClock == 50 || (__builtin_popcountl(board.occupations[PieceNameAny]) <= 5 && evaluations[0]->scorecp == 0) || repetition3x) {
    strcpy(result, "1/2-1/2");    
    score_white += 0.5;
    score_black += 0.5;
  }
  fprintf(file, "[Event \"Match of Champions\"]\n[Site \"sv Beruta\"]\n[Date \"2025.06.27\"]\n[Round \"%d\"]\n[White \"%s\"]\n[Black \"%s\"]\n[Result \"%s\"]\n\n", n, white->id, black->id, result);

	char * token = strtok(sanMoves, " ");
	int moveNumber = 1;
	bool whiteMove = true;
	while (token) {
	  if (whiteMove) {
      fprintf(file, "%d.%s ", moveNumber, token);
      whiteMove = false;
    } else {
      fprintf(file, "%s ", token);
      whiteMove = true; 
      moveNumber++;       
    }
		token = strtok(NULL, " ");	  
  }
  fprintf(file, "%s\n\n", result);
}

int main(int argc, char ** argv) {
  int n = 0; //number of simulations of NUMBER_OF_GAMES each
  init_magic_bitboards();
  evaluations[0] = &evaluation;
  evaluations[0]->maxPlies = 1; //we just need the next move  

  white = initChessEngine(WHITE_PATH, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, 1, false, false, ELO_WHITE);
  if (!white) fprintf(stderr, "tournament main() error: failed to init chessEngine %s for white\n", WHITE_PATH);
  //else fprintf(stderr, "initilized chess engine %s for white\n", white->id);
  
  black = initChessEngine(BLACK_PATH, MOVETIME, DEPTH, 128, 1, SYZYGY_PATH, 1, true, true, ELO_BLACK);
  if (!black) fprintf(stderr, "tournament main() error: failed to init chessEngine %s for black\n", BLACK_PATH);
  //else fprintf(stderr, "initilized chess engine %s for black\n", black->id);

  float elo_whites[5 * 5 * 7 * 6];
  float elo_blacks[5 * 5 * 7 * 6];
  float score_whites[5 * 5 * 7 * 6];
  float score_blacks[5 * 5 * 7 * 6];

	for (int m = 0; m < 5; m++) {
	  white->optionSpin[DirichletEpsilon].value = 15 + m * 5; //0..50; for testing 15..35 with step 5; default 25
	for (int l = 0; l < 5; l++) { // 1..9 with step 2; default 3
	  white->optionSpin[DirichletAlpha].value = 1 + l * 2;
	for (int k = 0; k < 7; k++) { //0..200; default 80. For testing 60..120 with step 10
	  white->optionSpin[ExplorationConstant].value = 60 + k * 10;
	for (int j = 0; j < 6; j++) { //0..100; default 80. For testing 50..100 with step 10
	  white->optionSpin[ProbabilityMass].value = 50 + j * 10;
	  n++;
	  char suffix[13];
	  sprintf(suffix, "-%d.pgn", n);
    filename[0] = 0;
	  strcat(filename, PGN_FILE);
	  strcat(filename, suffix);
  	file = fopen(filename, "a");
  	if (!file) {
  		fprintf(stderr, "error: failed to open/create a file %s: %s\n", filename, strerror(errno));
  		exit(1);
  	}
  	fprintf(file, "Test chess engine options:\nDirichletEpsilon %.2f\nDirichletAlpha %.2f\nExplorationConstant %.1f\nProbabilityMass %lld%%\n\n", (float)white->optionSpin[DirichletEpsilon].value / 100.0, (float)white->optionSpin[DirichletAlpha].value / 100.0, (float)white->optionSpin[ExplorationConstant].value / 100.0, white->optionSpin[ProbabilityMass].value);
    e_white = 1 / (1 + pow(10, (double)(ELO_BLACK - ELO_WHITE) / 400.0)); //expected white score
    e_black = 1.0 - e_white; //expected black score
    score_white = 0;
    score_black = 0;
    for (int i = 0; i < NUMBER_OF_GAMES; i++) {
      playGame((n - 1) * NUMBER_OF_GAMES + i + 1);
    }
    elo_white = round(ELO_WHITE + K_FACTOR * (score_white - e_white * NUMBER_OF_GAMES));
    if (n < sizeof(elo_whites)) {
      elo_whites[n] = elo_white;
      score_whites[n] = score_white;
    }
    else fprintf(file, "n (%d) > sizeof(elo_whites) (%lu)\n", n, sizeof(elo_whites));
    elo_black = round(ELO_BLACK + K_FACTOR * (score_black - e_black * NUMBER_OF_GAMES));
    if (n < sizeof(elo_blacks)) {
      elo_blacks[n] = elo_black;
      score_blacks[n] = score_black;
    }  
    else fprintf(file, "n (%d) > sizeof(elo_blacks) (%lu)\n", n, sizeof(elo_blacks));
    fprintf(file, "Elo white %.0f, elo black %.0f after %d games (%.1f : %.1f)\n", elo_white, elo_black, NUMBER_OF_GAMES, score_white, score_black);
    fclose(file);
  }
  }
  }
  }
  quit(white);
  quit(black);
  releaseChessEngine(white);
  releaseChessEngine(black);
  cleanup_magic_bitboards();
  float best_elo_white = 0;
  float best_elo_black = 0;
  int best_n_white = -1;
  int best_n_black = -1;
  for (int i = 0; i < n; i++) {
    if (elo_whites[i] > best_elo_white) {
      best_elo_white = elo_whites[i];
      best_n_white = i;
    }
    if (elo_blacks[i] > best_elo_black) {
      best_elo_black = elo_blacks[i];
      best_n_black = i;
    }
  }
  fprintf(stdout, "Best options for white are in %s-%d.pgn. Score %.1f : %.1f\n", PGN_FILE, best_n_white, score_whites[best_n_white], score_blacks[best_n_white]);
  fprintf(stdout, "Best options for black are in %s-%d.pgn. Score %.1f : %.1f\n", PGN_FILE, best_n_black, score_blacks[best_n_black], score_blacks[best_n_black]);
  
  return 0;
}

#ifdef __cplusplus
}
#endif
