#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nnue/bitboard.h"
#include "libchess.h"

bool isEmptyLine(const char * line) {
	size_t len = strlen(line);
	for (char c = 0; c < len; c++) {
		if (!isspace(line[c])) return false;
	}
	return true;
}

void stripGameResult(Game& game) {
	char * endOfMoves = strstr(game.sanMoves, "1-0");
	if (!endOfMoves) {
		endOfMoves = strstr(game.sanMoves, "0-1");
		if (!endOfMoves) {
			endOfMoves = strstr(game.sanMoves, "1/2-1/2");
			if (!endOfMoves) {
				endOfMoves = strstr(game.sanMoves, "*");
				if (endOfMoves) {
      	  if (strcmp(game.tags[Result], "*") != 0) {
    	      printf("stripGameResult() warning: PGN tag Result \'%s\' does not match the end of moves result \'*\' - updated the tag!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", game.tags[Result], game.tags[Event], game.tags[Site], game.tags[Date], game.tags[Round], game.tags[White], game.tags[Black]);
  	        strcpy(game.tags[Result], "*");
  	      }
  	    } else {
  	      printf("stripGameResult() warning: missing the end of moves result!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n[Result \"%s\"]\n", game.tags[Event], game.tags[Site], game.tags[Date], game.tags[Round], game.tags[White], game.tags[Black], game.tags[Result]);
  	    }
			} else {
    	  if (strcmp(game.tags[Result], "1/2-1/2") != 0) {
  	      printf("stripGameResult() warning: PGN tag Result \'%s\' does not match the end of moves result \'1/2-1/2\' - updated the tag!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", game.tags[Result], game.tags[Event], game.tags[Site], game.tags[Date], game.tags[Round], game.tags[White], game.tags[Black]);
  	      strcpy(game.tags[Result], "1/2-1/2");
  	    }
			}
		} else {
      if (strcmp(game.tags[Result], "0-1") != 0) {
  	    printf("stripGameResult() warning: PGN tag Result \'%s\' does not match the end of moves result \'0-1\' - updated the tag!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", game.tags[Result], game.tags[Event], game.tags[Site], game.tags[Date], game.tags[Round], game.tags[White], game.tags[Black]);
  	    strcpy(game.tags[Result], "0-1");
  	  }
		}
	} else {
      if (strcmp(game.tags[Result], "1-0") != 0) {
  	    printf("stripGameResult() warning: PGN tag Result \'%s\' does not match the end of moves result \'1-0\' - updated the tag!\n[Event \"%s\"]\n[Site \"%s\"]\n[Date \"%s\"]\n[Round \"%s\"]\n[White \"%s\"]\n[Black \"%s\"]\n", game.tags[Result], game.tags[Event], game.tags[Site], game.tags[Date], game.tags[Round], game.tags[White], game.tags[Black]);
    	  strcpy(game.tags[Result], "1-0");
    	}
	}
	if (endOfMoves) game.sanMoves[endOfMoves - game.sanMoves] = '\0';
}

//strips annotations, variations, etc 
//returns number of plies
int normalizeMoves(char * moves) {
	const char excludeChars[] = { '\n', '\v', '\t', ' ', '!', '?', '\r', '\1', '\2', '\3', '\4', '\5', '\6', '\7', '\10', '\16', '\17', '\20', '\21', '\22', '\23', '\24', '\25', '\26', '\27', '\30', '\31', '\32', '\33', '\34', '\35', '\36', '\37' };
	bool moreThanOne = false, skipCur = false, skipPar = false, dollar = false;
	int count = 0;
	int n = 0, numberOfPlies = 0;
	char * s = strdup(moves);
	if (!s) {
	  printf("normalizeMoves() error: strdup returned NULL: %s moves: %s\n", strerror(errno), moves);
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
	int n = 0, numberOfPlies = 0;
	char * s = strdup(moves);
	if (!s) {
	  printf("movesOnly() error: strdup() returned NULL: %s. moves %s\n", strerror(errno), moves);
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
uint64_t countGames(FILE * file, const char * firstLine, uint64_t gameStartPositions[], uint64_t maxNumberOfGames) {
	uint64_t numberOfGames = 0;
	char line[8];
	int res;
	long long pos = ftell(file);
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
int initEcoLines(const char * ecoFileName, struct EcoLine ** ecoLines) {
  uint64_t numberOfEcoLines = 0;
	FILE * ecoFile = fopen(ecoFileName, "r");
	if (!ecoFile) {
		printf("initEcoLines() warning: failed to open a ECO file %s, %s\n", ecoFileName, strerror(errno));
		return 1;
	}
	uint64_t ecoLinesStartPositions[MAX_NUMBER_OF_ECO_LINES];
	numberOfEcoLines = countGames(ecoFile, "[ECO ", ecoLinesStartPositions, MAX_NUMBER_OF_ECO_LINES);
	if (numberOfEcoLines > MAX_NUMBER_OF_ECO_LINES) {
		printf("initEcoLines() warning: number of lines in a eco file %s is %llu, which is greater than the maximum %d\n", ecoFileName, numberOfEcoLines, MAX_NUMBER_OF_ECO_LINES);
		numberOfEcoLines = MAX_NUMBER_OF_ECO_LINES;
	}
	rewind(ecoFile);

	char ecoLine[80];
	for (uint64_t i = 0; i < numberOfEcoLines; i++) {
		ecoLines[i] = (struct EcoLine *)malloc(sizeof(struct EcoLine));
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
		if (endOfLines) endOfLines[0] = '\0';

		//strip comments, variations and NAGs
		normalizeMoves(ecoLines[i]->sanMoves);

		//strip move numbers
		movesOnly(ecoLines[i]->sanMoves);
	}
	//classify = true;
	fclose(ecoFile);
	return numberOfEcoLines;
}

///<summary>
/// ECO classificator
///</summary>
void ecoClassify(Game& game, struct EcoLine ** ecoLine, int numberOfEcoLines) {
	size_t ecoLength = 0;
	int idx = -1;
	for (int i = 0; i < numberOfEcoLines; i++) {
		size_t newEcoLength = strlen(ecoLine[i]->sanMoves);
		if (strstr(game.sanMoves, ecoLine[i]->sanMoves) && newEcoLength > ecoLength) {
			ecoLength = newEcoLength;
			idx = i;
		}
	}
	if (idx >= 0) {
		if (ecoLine[idx]->tags[eECO][0] != '\0') {
			strncpy(game.tags[ECO], ecoLine[idx]->tags[eECO], MAX_TAG_VALUE_LEN);
		  //printf("%s\n", game.tags[ECO]);
		}
		if (ecoLine[idx]->tags[eOpening][0] != '\0') {
			strncpy(game.tags[Opening], ecoLine[idx]->tags[eOpening], MAX_TAG_VALUE_LEN);
		  //printf("%s\n", game.tags[Opening]);
		}
		if (ecoLine[idx]->tags[eVariation][0] != '\0') {
			strncpy(game.tags[Variation], ecoLine[idx]->tags[eVariation], MAX_TAG_VALUE_LEN);
		  //printf("%s\n", game.tags[Variation]);
    }
	}
}

int initGame(Game& game, FILE * file) {
	//Initialised: if the skip loop below ends because fgets() hit EOF rather than
	//because it found a move line, `line` is never written, and the strcat() that
	//follows would concatenate uninitialised stack memory into sanMoves.
	char line[80] = "";
	//read game PGN tags
	if (gTags(game.tags, file)) {
	  //fprintf(stderr, "initGame(): gTags() returned EOF\n");
	  return 1; //eof
	} 
	//fprintf(stderr, "initGame(): [Event \"%s\"]\n", game.tags[Event]);

	//skip empty lines and tag lines if any           
	bool haveMoveLine = false;
	while (fgets(line, sizeof line, file)) {
		if (!isEmptyLine(line) && line[0] != '[') { haveMoveLine = true; break; }
	}
	game.sanMoves[0] = '\0';

	//Read game moves until first empty line.
	//
	//Bounded: sanMoves is MAX_SAN_MOVES_LEN and a long game -- or one carrying long
	//comments, which are only stripped further down -- would otherwise run off the end
	//of the struct. Truncation loses moves; a plain strcat() corrupts memory.
	size_t used = 0;
	const size_t cap = sizeof game.sanMoves - 1;
	if (haveMoveLine) {
		const size_t n = strnlen(line, sizeof line);
		const size_t take = (n > cap - used) ? cap - used : n;
		memcpy(game.sanMoves + used, line, take);
		used += take;
		game.sanMoves[used] = '\0';
	}
	while (fgets(line, sizeof line, file)) {
		if (isEmptyLine(line)) break;
		const size_t n = strnlen(line, sizeof line);
		const size_t take = (n > cap - used) ? cap - used : n;
		if (!take) break;                 //sanMoves is full; keep what we have
		memcpy(game.sanMoves + used, line, take);
		used += take;
		game.sanMoves[used] = '\0';
	}

	//EOF is reported to the caller, but ONLY after the game that was just read has been
	//normalised.
	//
	//This used to return 1 here, before the three calls below. Reaching EOF while
	//reading the moves of a perfectly good game is the normal way a PGN file ends, so
	//the LAST game of every file came back raw: move numbers, comments, variations and
	//the result token all still in sanMoves, and numberOfPlies left at 0. It only looked
	//correct for files that happen to end with a blank line, because then the move loop
	//breaks on the empty line before EOF is set. Callers such as play_games.cpp use the
	//game regardless of the return value, so they were silently handing an unnormalised
	//move list to san2move() for one game per file.
	//
	//The contract is unchanged: non-zero still means end of file. What changed is that
	//the Game handed back is now always fully parsed. A caller that needs to know
	//whether a game was actually read should test numberOfPlies, not the return value.
	const int atEof = feof(file) ? 1 : 0;

	//strip the game result;
  stripGameResult(game);
  
	//strip comments, variations and NAGs
	normalizeMoves(game.sanMoves);

	//strip move numbers
	game.numberOfPlies = movesOnly(game.sanMoves);
	return atEof;
}

///<summary>
/// Plays a given game using Game struct
///</summary>
int playGame(Game& game) {
  int numberOfPlies = 0;
	struct Board board;
	char fenString[MAX_FEN_STRING_LEN];
	if (game.tags[FEN][0] == '\0') strcpy(fenString, startPos);
	else strncpy(fenString, game.tags[FEN],MAX_FEN_STRING_LEN);
	if (fen2board(board, fenString)) {
		printf("playGame() error: strtoboard() failed; FEN %s\n", fenString);
		return 1;
	}
	//getHash(&zh, &board);
	char * sanMoves = strdup(game.sanMoves);
	if (!sanMoves) {
		printf("playGame() error: strdup() returned NULL: %s. sanMoves %s\n", strerror(errno), game.sanMoves);
		return errno;
	}
	char * saveptr;
	char * token = strtok_r(sanMoves, " ", &saveptr);
	while (token) {
		if (strcmp(token, "1.") == 0) {
			free(sanMoves); 
			return 0;
		}
    Move move = {};
    int err = 0;
    if ((err = san2move(board, token, move))) {
    	printf("playGame() error: san2move() returned error %d\n", err);
    	exit(1);
    }
    isCheckMateStaleMate(board); //move generation to measure the performance of libchess
		ff_move(board, move);
		token = strtok_r(NULL, " ", &saveptr);
		numberOfPlies++;
	}
	free(sanMoves);
	if (numberOfPlies != game.numberOfPlies)
	  printf("playGame() error: numberOfPlies (%d) != game.numberOfPlies (%d), SAN moves %s\n", numberOfPlies, game.numberOfPlies, game.sanMoves);
	return 0;
}
