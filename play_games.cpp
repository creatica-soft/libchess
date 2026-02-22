//compile with c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess play_games.cpp -o play_games
#include <string>
#include <cstdio>
#include "libchess.h"

int main(int argc, char ** argv) {
  FILE * file = nullptr;
  std::string fileName;
  if (argc == 2) fileName = std::string(argv[1]);
  else {
    fprintf(stderr, "Usage: play_games <pgn_file>\n");
    exit(1);
  }
  file = fopen(fileName.c_str(), "r");
  if (!file) {
    fprintf(stderr, "main() error: fopen() returned NULL\n");
    exit(1);
  }
  init_magic_bitboards();
  struct Game game;
  unsigned long long game_number = 0;
  while(!feof(file)) {
    game_number++;
    int res = initGame(game, file);
    if (res && !feof(file)) {
      fprintf(stderr, "main() error: initGame() returned %d\n", res);
      exit(res);
    }
    //printf("initGame(%llu)\n", game_number);
    res = playGame(game);
    if (res) {
      fprintf(stderr, "main() error: playGame() returned %d\n", res);
      exit(res);
    }
    printf("%llu\n", game_number);
  }
  cleanup_magic_bitboards();
  fclose(file);
}