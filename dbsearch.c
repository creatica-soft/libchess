#define _POSIX_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sqlite3.h>
#include "libchess.h"

int dbinit(sqlite3 * pDb, int cacheSizeKB, int mmapSizeB, bool journalMode) {
  int res, num;
  char stmt[32];
  
  res = sqlite3_exec(pDb, "create table if not exists games (hash integer, constraint hash_game primary key (hash) on conflict ignore) without rowid;", NULL, NULL, NULL);
  if (res != SQLITE_OK) {
    printf("dbinit: sqlite3_exec(create table if not exists games (hash integer, constraint hash_game primary key (hash) on conflict ignore) without rowid;) returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_exec(pDb, "create table if not exists next_moves (hash integer, next_move varchar(7), score smallint, games int, constraint hash_move primary key (hash, next_move) on conflict ignore) without rowid;", NULL, NULL, NULL);
  if (res != SQLITE_OK) {
    printf("dbinit: sqlite3_exec(create table if not exists next_moves (hash integer, next_move varchar(7), score smallint, games int, constraint hash_move primary key (hash, next_move) on conflict ignore) without rowid;) returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }
  num = sprintf(stmt, "pragma journal_mode = %s;", journalMode ? "on" : "off");
  res = sqlite3_exec(pDb, stmt, NULL, NULL, NULL);
  if (res != SQLITE_OK) {
    printf("dbinit: sqlite3_exec(pragma journal_mode = %s;) returned %d (%s)\n", journalMode ? "on" : "off", res, sqlite3_errstr(res));
    return 1;
  }
  num = sprintf(stmt, "pragma cache_size = %d;", cacheSizeKB);
  res = sqlite3_exec(pDb, stmt, NULL, NULL, NULL);
  if (res != SQLITE_OK) {
    printf("dbinit: sqlite3_exec(pragma cache_size = %d;) returned %d (%s)\n", cacheSizeKB, res, sqlite3_errstr(res));
    return 1;
  }
  num = sprintf(stmt, "pragma mmap_size = %d;", mmapSizeB);
  res = sqlite3_exec(pDb, stmt, NULL, NULL, NULL);
  if (res != SQLITE_OK) {
    printf("dbinit: sqlite3_exec(pragma mmap_size = %d;) returned %d (%s)\n", mmapSizeB, res, sqlite3_errstr(res));
    return 1;
  }
	return 0;
}

int dbsearch(sqlite3 * pDb, unsigned long long hash, enum Color sideToMove, int multiPV, double rareMoveThreshold, char ** best_move) {
  int res, num;
  sqlite3_stmt *pStmt;
  char stmt[128];
  double totalGames;
  if (!pDb) {
    printf("dbsearch: sqlite3 connection handle is NULL");
    return 1;    
  }

  num = sprintf(stmt, "select total(games) from next_moves where hash=?1;");
  res = sqlite3_prepare_v2(pDb, stmt, num + 1, &pStmt, NULL);
  if (res != SQLITE_OK) {
    printf("dbsearch: sqlite3_prepare_v2(pDb, %s, %lu, ) returned %d (%s)\n", stmt, strlen(stmt) + 1, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_bind_int64(pStmt, 1, hash);
  if (res != SQLITE_OK) {
    printf("dbsearch: sqlite3_bind_int64(pStmt, 1, %lu) returned %d (%s)\n", hash, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_step(pStmt);
  if (res == SQLITE_ERROR) {
    printf("dbsearch: sqlite3_step(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }
  totalGames = sqlite3_column_double(pStmt, 0);
  res = sqlite3_finalize(pStmt);
  if (res != SQLITE_OK) {
    printf("dbsearch: sqlite3_finalize(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }

  if (sideToMove == ColorBlack) {
    num = sprintf(stmt, "select next_move from next_moves where hash=?1 and games / ?3 > ?4 order by score * 1.0 / games asc limit ?2;");
  }
  else {//colorWhite
    num = sprintf(stmt, "select next_move from next_moves where hash=?1 and games / ?3 > ?4 order by score * 1.0 / games desc limit ?2;");
  }
  res = sqlite3_prepare_v2(pDb, stmt, num + 1, &pStmt, NULL);
  if (res != SQLITE_OK) {
    printf("dbsearch: sqlite3_prepare_v2(pDb, %s, %lu, ) returned %d (%s)\n", stmt, strlen(stmt) + 1, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_bind_int64(pStmt, 1, hash);
  if (res != SQLITE_OK) {
    printf("dbsearch: sqlite3_bind_int64(pStmt, 1, %lu) returned %d (%s)\n", hash, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_bind_int(pStmt, 2, multiPV);
  if (res != SQLITE_OK) {
    printf("dbsearch: sqlite3_bind_int(pStmt, 2, %d) returned %d (%s)\n", multiPV, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_bind_double(pStmt, 3, totalGames);
  if (res != SQLITE_OK) {
    printf("dbsearch: sqlite3_bind_double(pStmt, 3, %f) returned %d (%s)\n", totalGames, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_bind_double(pStmt, 4, rareMoveThreshold);
  if (res != SQLITE_OK) {
    printf("dbsearch: sqlite3_bind_double(pStmt, 4, %f) returned %d (%s)\n", rareMoveThreshold, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_step(pStmt);
  int i = 0;
  while (res == SQLITE_ROW) {
    sprintf(best_move[i++], "%s", sqlite3_column_text(pStmt, 0));
    //if (i >= multiPV) break;
    res = sqlite3_step(pStmt);
  }
  if (res == SQLITE_ERROR) {
    printf("dbsearch: sqlite3_step(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_finalize(pStmt);
  if (res != SQLITE_OK) {
    printf("dbsearch: sqlite3_finalize(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }
  
  return 0;
}

int dbbegin(sqlite3 * pDb) {
  if (pDb == NULL) {
    printf("dbbegin: sqlite3 connection handle is NULL");
    return 1;    
  }
  int res = sqlite3_exec(pDb, "begin deferred transaction;", NULL, NULL, NULL);
  if (res != SQLITE_OK) {
    printf("dbbegin: sqlite3_exec(pDb, 'begin deferred transaction;') returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }
  return 0;
}

int dbcommit(sqlite3 * pDb) {
  if (pDb == NULL) {
    printf("dbcommit: sqlite3 connection handle is NULL");
    return 1;    
  }
  int res = sqlite3_exec(pDb, "commit transaction;", NULL, NULL, NULL);
  if (res != SQLITE_OK) {
    printf("dbcommit: sqlite3_exec(pDb, 'commit transaction;') returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }
  return 0;
}

int dbsearchGame(sqlite3 * pDb, unsigned long long gameHash) {
  int res, num;
  sqlite3_stmt *pStmt;
  char stmt[128];
  if (pDb == NULL) {
    printf("dbsearchGame: sqlite3 connection handle is NULL");
    return -1;    
  }
  num = sprintf(stmt, "select hash from games where hash=?1");
  res = sqlite3_prepare_v2(pDb, stmt, num + 1, &pStmt, NULL);
  if (res != SQLITE_OK) {
    printf("dbsearchGame: sqlite3_prepare_v2(pDb, %s, %lu, ) returned %d (%s)\n", stmt, strlen(stmt) + 1, res, sqlite3_errstr(res));
    return -1;
  }
  res = sqlite3_bind_int64(pStmt, 1, gameHash);
  if (res != SQLITE_OK) {
    printf("dbsearchGame: sqlite3_bind_int64(pStmt, 1, %lu) returned %d (%s)\n", gameHash, res, sqlite3_errstr(res));
    return -1;
  }
  int res2 = sqlite3_step(pStmt);
  //if (res2 == SQLITE_ROW)
  //  printf("%lx %s\n", (unsigned long long)sqlite3_column_int64(pStmt, 0), sqlite3_column_text(pStmt, 1));
  res = sqlite3_finalize(pStmt);
  if (res != SQLITE_OK) {
    printf("dbsearchGame: sqlite3_finalize(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
    return -1;
  }
  if (res2 == SQLITE_ROW) {
    return 0; //game already exists, skip it
  }
  //else insert it
  num = sprintf(stmt, "insert into games (hash) values (?1)");
  res = sqlite3_prepare_v2(pDb, stmt, num + 1, &pStmt, NULL);
  if (res != SQLITE_OK) {
    printf("dbsearchGame: sqlite3_prepare_v2(pDb, %s, %lu, ) returned %d (%s)\n", stmt, strlen(stmt) + 1, res, sqlite3_errstr(res));
    return -1;
  }
  res = sqlite3_bind_int64(pStmt, 1, gameHash);
  if (res != SQLITE_OK) {
    printf("dbsearchGame: sqlite3_bind_int64(pStmt, 1, %lu) returned %d (%s)\n", gameHash, res, sqlite3_errstr(res));
    return -1;
  }
  res = sqlite3_step(pStmt);
  if (res != SQLITE_DONE) {
    printf("dbsearchGame: sqlite3_step(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
    return -1;
  }
  res = sqlite3_finalize(pStmt);
  if (res != SQLITE_OK) {
    printf("dbsearchGame: sqlite3_finalize(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
    return -1;
  }
  return 1;
}  

int dbupdate(sqlite3 * pDb, unsigned long long hash, char * next_move, int score, unsigned long long gameHash) {
  int res, _score, games, num;
  sqlite3_stmt *pStmt;
  char stmt[128];
  if (pDb == NULL) {
    printf("dbupdate: sqlite3 connection handle is NULL");
    return 1;    
  }
  num = sprintf(stmt, "select score, games from next_moves where hash=?1 and next_move=?2");
  res = sqlite3_prepare_v2(pDb, stmt, num + 1, &pStmt, NULL);
  if (res != SQLITE_OK) {
    printf("dbupdate: sqlite3_prepare_v2(pDb, %s, %lu, ) returned %d (%s)\n", stmt, strlen(stmt) + 1, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_bind_int64(pStmt, 1, hash);
  if (res != SQLITE_OK) {
    printf("dbupdate: sqlite3_bind_int64(pStmt, 1, %lu) returned %d (%s)\n", hash, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_bind_text(pStmt, 2, next_move, strlen(next_move) + 1, SQLITE_STATIC);
  if (res != SQLITE_OK) {
    printf("dbupdate: sqlite3_bind_text(pStmt, 2, %s, %lu, SQLITE_STATIC) returned %d (%s)\n", next_move, strlen(next_move) + 1, res, sqlite3_errstr(res));
    return 1;
  }
  res = sqlite3_step(pStmt);
  if (res == SQLITE_ROW) {
    _score = sqlite3_column_int(pStmt, 0) + score;
    games = sqlite3_column_int(pStmt, 1) + 1;
    res = sqlite3_finalize(pStmt);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_finalize(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
      return 1;
    }
    num = sprintf(stmt, "update next_moves set score=?1, games=?2 where hash=?3 and next_move=?4");
    res = sqlite3_prepare_v2(pDb, stmt, num + 1, &pStmt, NULL);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_prepare_v2(pDb, %s, %lu, ) returned %d (%s)\n", stmt, strlen(stmt) + 1, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_bind_int(pStmt, 1, _score);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_bind_int(pStmt, 1, %d) returned %d (%s)\n", score, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_bind_int(pStmt, 2, games);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_bind_text(pStmt, 2, %d) returned %d (%s)\n", games, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_bind_int64(pStmt, 3, hash);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_bind_int64(pStmt, 3, %lu) returned %d (%s)\n", hash, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_bind_text(pStmt, 4, next_move, strlen(next_move) + 1, SQLITE_STATIC);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_bind_text(pStmt, 4, %s, %lu, SQLITE_STATIC) returned %d (%s)\n", next_move, strlen(next_move) + 1, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_step(pStmt);
    if (res == SQLITE_ERROR) {
      printf("dbupdate: sqlite3_step(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
      return 1;
    }
  } else {
    res = sqlite3_finalize(pStmt);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_finalize(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
      return 1;
    }
    num = sprintf(stmt, "insert into next_moves (hash, next_move, score, games) values (?3,?4,?1,?2)");
    res = sqlite3_prepare_v2(pDb, stmt, num + 1, &pStmt, NULL);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_prepare_v2(pDb, %s, %lu, ) returned %d (%s)\n", stmt, strlen(stmt) + 1, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_bind_int(pStmt, 1, score);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_bind_int(pStmt, 1, %d) returned %d (%s)\n", score, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_bind_int(pStmt, 2, 1);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_bind_text(pStmt, 2, %d) returned %d (%s)\n", games, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_bind_int64(pStmt, 3, hash);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_bind_int64(pStmt, 3, %lu) returned %d (%s)\n", hash, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_bind_text(pStmt, 4, next_move, strlen(next_move) + 1, SQLITE_STATIC);
    if (res != SQLITE_OK) {
      printf("dbupdate: sqlite3_bind_text(pStmt, 4, %s, %lu, SQLITE_STATIC) returned %d (%s)\n", next_move, strlen(next_move) + 1, res, sqlite3_errstr(res));
      return 1;
    }
    res = sqlite3_step(pStmt);
    if (res == SQLITE_ERROR) {
      printf("dbupdate: sqlite3_step(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
      return 1;
    }
  }
  res = sqlite3_finalize(pStmt);
  if (res != SQLITE_OK) {
    printf("dbupdate: sqlite3_finalize(pStmt) returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }
  return 0;
}

int dbclose(sqlite3 * pDb) {
  int res;
  res = sqlite3_close(pDb);
  if (res != SQLITE_OK) {
    printf("dbclose: sqlite3_close(pDb) returned %d (%s)\n", res, sqlite3_errstr(res));
    return 1;
  }
  return 0;
}