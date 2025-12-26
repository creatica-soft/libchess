#ifdef __cplusplus
extern "C" {
#endif
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

bool isEmptyLine(const char * line) {
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
unsigned long long countGames(FILE * file, const char * firstLine, unsigned long long gameStartPositions[], unsigned long long maxNumberOfGames) {
	unsigned long long numberOfGames = 0;
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
  unsigned long long numberOfEcoLines = 0;
	FILE * ecoFile = fopen(ecoFileName, "r");
	if (!ecoFile) {
		printf("initEcoLines() warning: failed to open a ECO file %s, %s\n", ecoFileName, strerror(errno));
		return 1;
	}
	unsigned long long ecoLinesStartPositions[MAX_NUMBER_OF_ECO_LINES];
	numberOfEcoLines = countGames(ecoFile, "[ECO ", ecoLinesStartPositions, MAX_NUMBER_OF_ECO_LINES);
	if (numberOfEcoLines > MAX_NUMBER_OF_ECO_LINES) {
		printf("initEcoLines() warning: number of lines in a eco file %s is %llu, which is greater than the maximum %d\n", ecoFileName, numberOfEcoLines, MAX_NUMBER_OF_ECO_LINES);
		numberOfEcoLines = MAX_NUMBER_OF_ECO_LINES;
	}
	rewind(ecoFile);

	char ecoLine[80];
	for (unsigned long long i = 0; i < numberOfEcoLines; i++) {
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

///<summary>
/// Plays a given game using Game struct
///</summary>
int playGame(struct Game * game) {
  int numberOfPlies = 0;
	struct Board board;
	struct Fen fen;
	//struct ZobristHash zh;//, zh2;
	//zobristHash(&zh);
	//zobristHash(&zh2); //for debugging
	char fenString[MAX_FEN_STRING_LEN];
	if (game->tags[FEN][0] == '\0') strcpy(fenString, startPos);
	else strncpy(fenString, game->tags[FEN],MAX_FEN_STRING_LEN);
	if (strtofen(&fen, fenString)) {
		printf("playGame() error: strtofen() failed; FEN %s\n", fenString);
		return 1;
	}
	if (fentoboard(&fen, &board)) {
		printf("playGame() error: fentoboard() failed; FEN %s\n", fen.fenString);
		return 1;
	}
	//getHash(&zh, &board);
	char * sanMoves = strdup(game->sanMoves);
	if (!sanMoves) {
		printf("playGame() error: strdup() returned NULL: %s. sanMoves %s\n", strerror(errno), game->sanMoves);
		return errno;
	}
	char * saveptr;
	char * token = strtok_r(sanMoves, " ", &saveptr);
	while (token) {
   	struct Move move;
		initMove(&move, &board, token);
		makeMove(&move);
		//updateHash(&board, &move);
		//unsigned long long hash = board.zh->hash;
		//unsigned long long hash2 = board.zh->hash2;
		//getHash(&zh2, &board);
		//assert(hash != board.zh->hash || hash2 != board.zh->hash2);
		//reconcile(&board);
		token = strtok_r(NULL, " ", &saveptr);
		numberOfPlies++;
	}
	free(sanMoves);
	if (numberOfPlies != game->numberOfPlies)
	  printf("playGame() error: numberOfPlies (%d) != game->numberOfPlies (%d), SAN moves %s\n", numberOfPlies, game->numberOfPlies, game->sanMoves);
	return 0;
}

#ifdef __cplusplus
}
#endif
