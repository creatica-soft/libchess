// compile with c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o tournament tournament.cpp

#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <format>
#include <math.h>
#include <unordered_map>
#include "libchess.h"

#define CREATICA_PATH "/Users/ap/libchess/creatica"
#define STOCKFISH_PATH "/Users/ap/libchess/stockfish-macos-m1-apple-silicon"
#define SYZYGY_PATH "/Users/ap/syzygy"
#define PGN_FILE "creatica-stockfish-temp"
#define ELO_CREATICA 2500
#define ELO_STOCKFISH 2500
#define K_FACTOR 20 // 40 for new players (under 30 games), 20 for established players under 2400, 10 for masters (2400+) 
#define MOVETIME 1000
#define DEPTH 0
#define HASH 1024
#define THREADS 8
#define NUMBER_OF_GAMES 10

#ifdef __cplusplus
extern "C" {
#endif

//Globals
double e_creatica, e_stockfish, score_creatica, score_stockfish;
float elo_creatica, elo_stockfish;
struct Engine * creatica = nullptr, * stockfish = nullptr;
struct Board board;
struct Fen fen;
struct ZobristHash zh;
struct Move move;
struct Evaluation evaluation;
struct Evaluation * evaluations[1] = { nullptr };
char sanMoves[4096] = "";
FILE * file = nullptr;
char filename[255];
  
void play_game(struct Engine * white, struct Engine * black, int n) {
  fprintf(stdout, "game %d, score creatica : stockfish %.1f : %.1f\n", n, score_creatica, score_stockfish);
  newGame(white);
  newGame(black);
  strtofen(&fen, startPos);
  fentoboard(&fen, &board);
  zobristHash(&zh);
  board.zh = &zh;
  sanMoves[0] = 0;
  std::unordered_map<unsigned long long, int> position_history;
  bool repetition3x = false;
  while (!board.isMate && !board.isStaleMate && board.fen->halfmoveClock != 100 && !(__builtin_popcountl(board.occupations[PieceNameAny]) <= 5 && evaluations[0]->scorecp == 0) && !repetition3x) {
    strncpy(white->position, board.fen->fenString, MAX_FEN_STRING_LEN);
    if (!position(white)) {
      fprintf(stderr, "error: position(white) returned false, fen %s\n", white->position);
      exit(2);
    }
    if (go(white, evaluations)) {
      fprintf(stderr, "error: go(white, evaluations) returned non-zero code, fen %s\n", white->position);
      exit(2);
    }
  	initMove(&move, &board, evaluations[0]->bestmove);
    strcat(sanMoves, move.sanMove);
    strcat(sanMoves, " ");
    makeMove(&move);
    fprintf(stdout, "game %d, score creatica : stockfish %.1f : %.1f, score cp %d\n", n, score_creatica, score_stockfish, evaluations[0]->scorecp);
    writeDebug(&board, false);
    updateHash(&board, &move);
    if (position_history[board.zh->hash] >= 2) { // 3rd occurrence
        repetition3x = true;
        break;
    }
    position_history[board.zh->hash]++;
    if (board.isMate || board.isStaleMate || board.fen->halfmoveClock == 100 && !(__builtin_popcountl(board.occupations[PieceNameAny]) <= 5 && evaluations[0]->scorecp == 0) || repetition3x) {
      //fprintf(stderr, "mate, stalemate or halfmoveClock = 100\n");
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
  	initMove(&move, &board, evaluations[0]->bestmove);
    strcat(sanMoves, move.sanMove);
    strcat(sanMoves, " ");
    makeMove(&move);
    fprintf(stdout, "game %d, score creatica : stockfish %.1f : %.1f, score cp %d\n", n, score_creatica, score_stockfish, -evaluations[0]->scorecp);
    writeDebug(&board, false);
    updateHash(&board, &move);
    if (position_history[board.zh->hash] >= 2) { // 3rd occurrence
        repetition3x = true;
        break;
    }
    position_history[board.zh->hash]++;
  }
  position_history.clear();
  
  char result[8];
  int eloWhite, eloBlack;
  if (strstr(black->id, "Stockfish")) {
    eloWhite = ELO_CREATICA;
    eloBlack = ELO_STOCKFISH;
  } else {
    eloWhite = ELO_STOCKFISH;
    eloBlack = ELO_CREATICA;
  }
  if (board.isMate) {
    if (board.fen->sideToMove == ColorWhite) {
      strcpy(result, "0-1");
      if (strstr(black->id, "Stockfish")) {
        score_stockfish += 1;
      }
      else {
        score_creatica += 1.0;
      }
    }
    else {
      strcpy(result, "1-0");        
      if (strstr(white->id, "Creatica")) {
        score_creatica += 1.0;
      }
      else {
        score_stockfish += 1;
      }
    }
  } 
  else if (board.isStaleMate || board.fen->halfmoveClock == 100 || (__builtin_popcountl(board.occupations[PieceNameAny]) <= 5 && evaluations[0]->scorecp == 0) || repetition3x) {
    strcpy(result, "1/2-1/2");    
    score_creatica += 0.5;
    score_stockfish += 0.5;
  }
  auto now = std::chrono::system_clock::now();
  std::string date = std::format("{:%Y.%m.%d}", now);
  fprintf(file, "[Event \"Match of Champions\"]\n[Site \"sv Beruta\"]\n[Date \"%s\"]\n[Round \"%d\"]\n[White \"%s\"]\n[Black \"%s\"]\n[WhiteElo \"%d\"]\n[BlackElo \"%d\"]\n[Result \"%s\"]\n\n", date.c_str(), n, white->id, black->id, eloWhite, eloBlack, result);

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

#ifdef __cplusplus
}
#endif

int main(int argc, char ** argv) {
  int n = 0; //number of simulations of NUMBER_OF_GAMES each
  init_magic_bitboards();
  evaluations[0] = &evaluation;
  evaluations[0]->maxPlies = 1; //we just need the next move  

  creatica = initChessEngine(CREATICA_PATH, 2 * MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, 1, false, false, ELO_CREATICA);
  if (!creatica) fprintf(stderr, "tournament main() error: failed to init chessEngine %s for creatica\n", CREATICA_PATH);
  //else fprintf(stderr, "initilized chess engine %s for creatica\n", creatica->id);
  
  stockfish = initChessEngine(STOCKFISH_PATH, MOVETIME, DEPTH, 128, 1, SYZYGY_PATH, 1, true, true, ELO_STOCKFISH);
  if (!stockfish) fprintf(stderr, "tournament main() error: failed to init chessEngine %s for black\n", STOCKFISH_PATH);
  //else fprintf(stderr, "initilized chess engine %s for stockfish\n", stockfish->id);

  int jj = 5; //number of tests for j loop
  float elos_creatica[jj];
  float elos_stockfish[jj];
  float scores_creatica[jj];
  float scores_stockfish[jj];

//	for (int l = 0; l < ll; l++) { // 3..7 with step 1; default 5
//	  creatica->optionSpin[Noise].value = 3 + l;
//	for (int k = 0; k < kk; k++) { //80..120 with step 20
	  //creatica->optionSpin[ExplorationMax].value = 80 + k * 20;
	  creatica->optionCheck[FinalInfoLines].value = false;
	  creatica->optionCheck[IntermittentInfoLines].value = false;
	  creatica->optionSpin[VirtualLoss].value = 4;
	  creatica->optionSpin[ExplorationMax].value = 120;
	  creatica->optionSpin[ExplorationMin].value = 40;
	  creatica->optionSpin[ProbabilityMass].value = 100;
	  creatica->optionSpin[ExplorationDepthDecay].value = 6;
	  //creatica->optionSpin[Noise].value = 3;
	  creatica->optionSpin[EvalScale].value = 4;
	  //creatica->optionSpin[Temperature].value = 60; //55 - 60 sweet spot
	for (int j = 0; j < jj; j++) { //50..70, step 5
	  creatica->optionSpin[Temperature].value = 50 + j * 5;
	  setOptions(creatica);
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
  	fprintf(file, "Test chess engine options:\nExplorationMin %.1f\nExplorationMax %.1f\nExplorationDepthDecay %.1f\nProbabilityMass %lld%%\nVirtualLoss %lld\nEvalScale %lld\nTemperature %lld\n\n", (float)creatica->optionSpin[ExplorationMin].value / 100.0, (float)creatica->optionSpin[ExplorationMax].value / 100.0, (float)creatica->optionSpin[ExplorationDepthDecay].value / 100.0, creatica->optionSpin[ProbabilityMass].value, creatica->optionSpin[VirtualLoss].value, creatica->optionSpin[EvalScale].value, creatica->optionSpin[Temperature].value);
    e_creatica = 1 / (1 + pow(10, (double)(ELO_STOCKFISH - ELO_CREATICA) / 400.0)); //expected creatica score
    e_stockfish = 1.0 - e_creatica; //expected stockfish score
    score_creatica = 0;
    score_stockfish = 0;
    for (int i = 0; i < NUMBER_OF_GAMES; i++) {
      if (i % 2)
        play_game(stockfish, creatica, (n - 1) * NUMBER_OF_GAMES + i + 1);
      else
        play_game(creatica, stockfish, (n - 1) * NUMBER_OF_GAMES + i + 1);
    }
    elo_creatica = round(ELO_CREATICA + K_FACTOR * (score_creatica - e_creatica * NUMBER_OF_GAMES));
    if (n <= sizeof(elos_creatica)) {
      elos_creatica[n - 1] = elo_creatica;
      scores_creatica[n - 1] = score_creatica;
    }
    else fprintf(file, "n (%d) > sizeof(elos_creatica) (%llu)\n", n, sizeof(elos_creatica));
    elo_stockfish = round(ELO_STOCKFISH + K_FACTOR * (score_stockfish - e_stockfish * NUMBER_OF_GAMES));
    if (n <= sizeof(elos_stockfish)) {
      elos_stockfish[n - 1] = elo_stockfish;
      scores_stockfish[n - 1] = score_stockfish;
    }  
    else fprintf(file, "n (%d) > sizeof(elos_stockfish) (%llu)\n", n, sizeof(elos_stockfish));
    fprintf(file, "Elo creatica %.0f, elo stockfish %.0f after %d games (%.1f : %.1f)\n", elo_creatica, elo_stockfish, NUMBER_OF_GAMES, score_creatica, score_stockfish);
    fclose(file);
  }
//  }
//  }
  quit(creatica);
  quit(stockfish);
  releaseChessEngine(creatica);
  releaseChessEngine(stockfish);
  cleanup_magic_bitboards();
  float best_elo_creatica = 0;
  float best_elo_stockfish = 0;
  int best_n_creatica = -1;
  int best_n_stockfish = -1;
  for (int i = 0; i < n; i++) {
    if (elos_creatica[i] > best_elo_creatica) {
      best_elo_creatica = elos_creatica[i];
      best_n_creatica = i;
    }
    if (elos_stockfish[i] > best_elo_stockfish) {
      best_elo_stockfish = elos_stockfish[i];
      best_n_stockfish = i;
    }
  }
  fprintf(stdout, "Best options for creatica are in %s-%d.pgn. Score %.1f : %.1f\n", PGN_FILE, best_n_creatica + 1, scores_creatica[best_n_creatica], scores_stockfish[best_n_creatica]);
  fprintf(stdout, "Best options for stockfish are in %s-%d.pgn. Score %.1f : %.1f\n", PGN_FILE, best_n_stockfish + 1, scores_stockfish[best_n_stockfish], scores_creatica[best_n_stockfish]);
  
  return 0;
}
