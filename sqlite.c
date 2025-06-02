#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sqlite3.h>
#include "libchess.h"

//PRAGMA busy_timeout = milliseconds
//PRAGMA cache_size = pages //default is -2000 KB (or 512 pages); each page is 4096 bytes
//PRAGMA journal_mode = DELETE | TRUNCATE | PERSIST | MEMORY | WAL | OFF Default is DELETE 
//PRAGMA mmap_size=N bytes, default is 0 - disabled
sqlite3 * openDb(const char * filename, int flags) {
  sqlite3 * db;
  char * errmsg = NULL;
  
  int res = sqlite3_open_v2(filename, &db, flags, NULL);
  if (res != SQLITE_OK) {
     printf("openDb() error: sqlite3_open_v2() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(1);
  }
  res = sqlite3_exec(db, "pragma journal_mode=OFF", NULL, NULL, &errmsg);
  res = sqlite3_exec(db, "pragma cache_size=-1000000", NULL, NULL, &errmsg);
  res = sqlite3_exec(db, "PRAGMA mmap_size=1000000000", NULL, NULL, &errmsg);
  //res = sqlite3_exec(db, "pragma busy_timeout=1000", NULL, NULL, &errmsg);
  //res = sqlite3_threadsafe(); //if 0 - multi-threading is disabled, 2 - enabled (threads must have their own connections), 1 - serialized (no restrictions, all mutexes are compiled)
  //printf("openDb() sqlite3_threadsafe() returned %d\n", res); //MacOS has 2, so no db connection sharing between threads!
  return db;
}

void closeDb(sqlite3 * db) {
  int res = sqlite3_close_v2(db);
  if (res != SQLITE_OK) {
    printf("closeDb sqlite3_close_v2() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
    exit(1);
   }
}

int gameExists(sqlite3 * db, sqlite3_int64 hash) {
  bool gameFound = false;
  const char * sql = "select hash from games where hash=:hash;";
  sqlite3_stmt * query;
  struct timespec delay;
  delay.tv_sec = 0;
  delay.tv_nsec = 10;

  if (!db) {
    printf("gameExists() error: db is NULL\n");
    exit(1);
  }
  int res = sqlite3_prepare_v2(db, sql, -1, &query, NULL);
  if (res != SQLITE_OK) {
     printf("gameExists() error: sqlite3_prepare_v2() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(2);
  }
  res = sqlite3_bind_int64(query, sqlite3_bind_parameter_index(query, ":hash"), hash);
  if (res != SQLITE_OK) {
     printf("gameExists() error: sqlite3_bind_int64(hash) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     sqlite3_finalize(query);
     exit(3);
  }
  while (res != SQLITE_DONE) {
    res = sqlite3_step(query);
    while (res == SQLITE_BUSY) {
      nanosleep(&delay, NULL);
      res = sqlite3_step(query);
    }
    if (res == SQLITE_ROW) {
      gameFound = true;
      continue;
    }
    else if (res == SQLITE_ERROR) {
     printf("gameExists() error: sqlite3_step() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     sqlite3_finalize(query);
     exit(4);      
    }
  }
  res = sqlite3_finalize(query);
  if (res != SQLITE_OK) {
     printf("gameExists() error: sqlite3_finalize(query) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(5);
  }
  if (gameFound) return 1;
  else return 0;
}

int insertGame(sqlite3 * db, sqlite3_int64 hash, char * moves) {
  sqlite3_stmt * query;
  if (!db) {
    printf("insertGame() error: db is NULL\n");
    exit(1);
  }
  struct timespec delay;
  delay.tv_sec = 0;
  delay.tv_nsec = 10;
  
  const char * sql;
  if (!gameExists(db, hash)) {
    if (moves)
      sql = "insert into games(hash, moves) values(:hash, :moves);";
    else sql = "insert into games(hash) values(:hash);";
  }
  else return 1; //this error code is needed to avoid updating next_moves table
  int res = sqlite3_prepare_v2(db, sql, strlen(sql) + 1, &query, NULL);
  if (res != SQLITE_OK) {
     printf("insertGame() error: sqlite3_prepare_v2() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(2);
  }  
  res = sqlite3_bind_int64(query, sqlite3_bind_parameter_index(query, ":hash"), hash);
  if (res != SQLITE_OK) {
    printf("insertGame() error: sqlite3_bind_int64(hash) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
    sqlite3_finalize(query);
    exit(3);
  }
  if (moves) {
    res = sqlite3_bind_text(query, sqlite3_bind_parameter_index(query, ":moves"), moves, -1, SQLITE_TRANSIENT);
    if (res != SQLITE_OK) {
      printf("insertGame() error: sqlite3_bind_int64(moves) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
      sqlite3_finalize(query);
      exit(3);
    }    
  }
  res = sqlite3_step(query);
  while (res == SQLITE_BUSY) {
    nanosleep(&delay, NULL);
    res = sqlite3_step(query);
  }
  if (res != SQLITE_DONE) {
    printf("insertGame() error: sqlite3_step() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
    sqlite3_finalize(query);
    exit(4);      
  }
  res = sqlite3_finalize(query);
  if (res != SQLITE_OK) {
     printf("insertGame() error: sqlite3_finalize(query) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(5);
  }
  return 0;
}

int getNextMoves(sqlite3 * db, sqlite3_int64 hash, struct MoveScoreGames ** nextMove) {
  const char * sql = "select next_move, score, games, scorecp from next_moves where hash=:hash;";
  sqlite3_stmt * query = NULL;
  int numberOfMoves = 0;
  
  if (!db) {
    printf("getNextMoves() error: db is NULL\n");
    exit(1);
  }
  int res = sqlite3_prepare_v2(db, sql, strlen(sql) + 1,&query, NULL);
  if (res != SQLITE_OK) {
     printf("getNextMoves() error: sqlite3_prepare_v2() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(2);
  }  
  res = sqlite3_bind_int64(query, sqlite3_bind_parameter_index(query, ":hash"), hash);
  if (res != SQLITE_OK) {
     printf("getNextMoves() error: sqlite3_bind_int64(hash) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     sqlite3_finalize(query);
     exit(3);
  }
  int i = 0;
  while (res != SQLITE_DONE) {
    res = sqlite3_step(query);
    if (res == SQLITE_ROW) {
      nextMove[i] = malloc(sizeof(struct MoveScoreGames));
      strncpy(nextMove[i]->move, (const char *)sqlite3_column_text(query, 0), 6);
      nextMove[i]->score = sqlite3_column_int(query, 1);
      nextMove[i]->games = sqlite3_column_int(query, 2);
      nextMove[i++]->scorecp = sqlite3_column_int(query, 3);
      numberOfMoves++;
      if (i >= MAX_NUMBER_OF_NEXT_MOVES) {
        printf("getNextMoves() error: in sqlite3_step() too many moves returned, the max is %d\n", MAX_NUMBER_OF_NEXT_MOVES);
        sqlite3_finalize(query);
        exit(4);
      }
      continue;
    }
    else if (res == SQLITE_ERROR) {
     printf("getNextMoves() error: sqlite3_step() returned %d (%s): %s\n", res, sqlite3_errstr(res), sqlite3_errmsg(db));
     sqlite3_finalize(query);
     exit(5);      
    }
  }
  res = sqlite3_finalize(query);
  if (res != SQLITE_OK) {
     printf("getNextMoves() error: sqlite3_finalize(query) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(6);
  }
  return numberOfMoves;
}

void getNextMove(sqlite3 * db, sqlite3_int64 hash, const char * move, struct MoveScoreGames * nextMove) {
  const char * sql = "select next_move, score, games, scorecp from next_moves where hash=:hash and next_move=:move;";
  sqlite3_stmt * query = NULL;

  if (!db) {
    printf("getNextMove() error: db is NULL\n");
    exit(1);
  }
  int res = sqlite3_prepare_v2(db, sql, strlen(sql) + 1, &query, NULL);
  if (res != SQLITE_OK) {
     printf("getNextMove() error: sqlite3_prepare_v2() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(2);
  }  
  res = sqlite3_bind_int64(query, sqlite3_bind_parameter_index(query, ":hash"), hash);
  if (res != SQLITE_OK) {
     printf("getNextMove() error: sqlite3_bind_int64(hash) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     sqlite3_finalize(query);
     exit(3);
  }
  res = sqlite3_bind_text(query, sqlite3_bind_parameter_index(query, ":move"), move, strlen(move), SQLITE_TRANSIENT);
  if (res != SQLITE_OK) {
     printf("getNextMove() error: sqlite3_bind_text(move) returned %d (%s): %s\n", res, sqlite3_errstr(res), sqlite3_errmsg(db));
     sqlite3_finalize(query);
     exit(4);
  }
  while (res != SQLITE_DONE) {
    res = sqlite3_step(query);
    if (res == SQLITE_ROW) {
      strncpy(nextMove->move, (const char *)sqlite3_column_text(query, 0), 6);
      nextMove->score = sqlite3_column_int(query, 1);
      nextMove->games = sqlite3_column_int(query, 2);
      nextMove->scorecp = sqlite3_column_int(query, 3);
      continue;
    }
    else if (res == SQLITE_ERROR) {
     printf("getNextMove() error: sqlite3_step() returned %d (%s): %s\n", res, sqlite3_errstr(res), sqlite3_errmsg(db));
     sqlite3_finalize(query);
     exit(5);      
    }
  }
  res = sqlite3_finalize(query);
  if (res != SQLITE_OK) {
     printf("getNextMove() error: sqlite3_finalize(query) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(6);
  }
}

void updateNextMove(sqlite3 * db, sqlite3_int64 hash, const char * move, int gameResult, int scorecp) {
  sqlite3_stmt * query = NULL;
  const char * sql;
  struct MoveScoreGames nextMove;
  nextMove.move[0] = '\0';
  nextMove.score = 0;
  nextMove.games = 0;
  nextMove.scorecp = scorecp;
  if (!db) {
    printf("updateNextMove() error: db is NULL\n");
    exit(1);
  }
  struct timespec delay;
  delay.tv_sec = 0;
  delay.tv_nsec = 100;

  getNextMove(db, hash, move, &nextMove);
  if (nextMove.games == 0) {
    sql = "insert into next_moves(hash, next_move, score, games, scorecp) values(:hash, :move, :score, :games, :scorecp);";
    nextMove.games = 1;
  } else {
    sql = "update next_moves set score = :score, games = :games, scorecp = :scorecp where hash=:hash and next_move=:move;";
    nextMove.score += gameResult;
    nextMove.games += 1;
  }
  int res = sqlite3_prepare_v2(db, sql, strlen(sql) + 1, &query, NULL);
  if (res != SQLITE_OK) {
     printf("updateNextMove() error: sqlite3_prepare_v2() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(3);
  }  
  res = sqlite3_bind_int64(query, sqlite3_bind_parameter_index(query, ":hash"), hash);
  if (res != SQLITE_OK) {
    printf("updateNextMove() error: sqlite3_bind_int64(hash) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
    sqlite3_finalize(query);
    exit(4);
  }
  res = sqlite3_bind_text(query, sqlite3_bind_parameter_index(query, ":move"), move, strlen(move), SQLITE_TRANSIENT);
  if (res != SQLITE_OK) {
    printf("updateNextMove() error: sqlite3_bind_text(move) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
    sqlite3_finalize(query);
    exit(5);
  }
  res = sqlite3_bind_int(query, sqlite3_bind_parameter_index(query, ":score"), nextMove.score);
  if (res != SQLITE_OK) {
    printf("updateNextMove() error: sqlite3_bind_int(score) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
    sqlite3_finalize(query);
    exit(6);
  }
  res = sqlite3_bind_int(query, sqlite3_bind_parameter_index(query, ":games"), nextMove.games);
  if (res != SQLITE_OK) {
    printf("updateNextMove() error: sqlite3_bind_int(games) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
    sqlite3_finalize(query);
    exit(7);
  }
  res = sqlite3_bind_int(query, sqlite3_bind_parameter_index(query, ":scorecp"), nextMove.scorecp);
  if (res != SQLITE_OK) {
    printf("updateNextMove() error: sqlite3_bind_int(scorecp) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
    sqlite3_finalize(query);
    exit(7);
  }
  res = sqlite3_step(query);
  while (res == SQLITE_BUSY) {
    nanosleep(&delay, NULL);
    res = sqlite3_step(query);    
  }
  if (res != SQLITE_DONE) {
    printf("updateNextMove() error: sqlite3_step() returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
    sqlite3_finalize(query);
    exit(8);      
  }
  res = sqlite3_finalize(query);
  if (res != SQLITE_OK) {
     printf("updateNextMove() error: sqlite3_finalize(query) returned %d (%s): %s, ext err code %d\n", res, sqlite3_errstr(res), sqlite3_errmsg(db), sqlite3_extended_errcode(db));
     exit(6);
  }
}
