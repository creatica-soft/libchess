#pragma warning(disable:4996)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h> //use -pthread when compiling and linking
#include "libchess.h"

static unsigned long gameNumber = 0, numberOfGames, numberOfEcoLines = 0;
static long gameStartPositions[MAX_NUMBER_OF_GAMES];
static bool classify = false;
static int playGameResult[MAX_NUMBER_OF_THREADS];
static struct EcoLine * ecoLines[MAX_NUMBER_OF_ECO_LINES];
static pthread_mutex_t gameNumber_mutex = PTHREAD_MUTEX_INITIALIZER;
struct PlayGameContext {
	char fileName[255];
	int threadNumber;
};

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

///<summary>
/// plays a given pgn game
/// returns 0 on success, non-zero on error
///</summary>
int pgnGame(struct Game * game, bool generateZobristHash) {
	struct Move move;
	struct Board board;
	struct ZobristHash zh;

	struct Fen fen;
	char * fenString;
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
		zobristHash(&zh);
		getHash(&zh, &board);
		board.hash = zh.hash;
	}
	
	char * sanMoves = strdup(game->sanMoves);
	if (!sanMoves) {
		printf("pgnGame() error: strdup() returned %s\n", strerror(errno));
		return errno;
	}
	char * saveptr;
	char * token = strtok_r(sanMoves, " ", &saveptr);
	while (token)
	{
		if (initMove(&move, &board, token)) {
			printf("pgnGame() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
			free(sanMoves);
			return 1;
		}

		makeMove(&move);
		//reconcile(&board);
		if (generateZobristHash) {
			if (!updateHash(&zh, &board, &move))
				board.hash = zh.hash;
			else {
				printf("pgnGame() error: updateHash() returned non-zero value\n");
				free(sanMoves);
				return 1;
			}
		}
		if (board.isStaleMate && strcmp(game->tags[Result], "1/2-1/2") != 0) {
			printf("Game tag Result '%s' did not match the actual one '1/2-1/2'\n", game->tags[Result]);
			strcpy(game->tags[Result], "1/2-1/2");
		}
		else if (board.isMate && board.movingPiece.color == ColorWhite && strcmp(game->tags[Result], "1-0") != 0) {
			printf("Game tag Result '%s' did not match the actual one '1-0'\n", game->tags[Result]);
			strcpy(game->tags[Result], "1-0");
		}
		else if (board.isMate && board.movingPiece.color == ColorBlack && strcmp(game->tags[Result], "0-1") != 0) {
			printf("Game tag Result '%s' did not match the actual one '0-1'\n", game->tags[Result]);
			strcpy(game->tags[Result], "0-1");
		}
		token = strtok_r(NULL, " ", &saveptr);
	}
	free(sanMoves);
	return 0;
}

void * playGame(void * context) {
	struct Game game;
	char line[80];
	unsigned long gNumber;
	struct PlayGameContext * ctx = (struct PlayGameContext *)context;
	//for (unsigned long i = 0; i < numberOfGames; i++) {
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
		if (!endOfMoves) {
			endOfMoves = strstr(game.sanMoves, "0-1");
			if (!endOfMoves) {
				endOfMoves = strstr(game.sanMoves, "1/2-1/2");
				if (!endOfMoves) {
					endOfMoves = strstr(game.sanMoves, "*");
				}
			}
		}
		//strip the game result;
		if (endOfMoves) game.sanMoves[endOfMoves - game.sanMoves] = '\0';
		//printf("playGame() %lu: game.sanMoves without result %s\n", gNumber, game.sanMoves);

		//strip comments, variations and NAGs
		normalizeMoves(game.sanMoves);
		//printf("playGame() %lu: normalized game.sanMoves %s\n", gNumber, game.sanMoves);

		//strip move numbers
		movesOnly(game.sanMoves);
		//printf("playGame() %lu: game.sanMoves only %s\n", gNumber, game.sanMoves);

		struct Fen fen;
		char * fenString;
		if (game.tags[FEN][0] == '\0') fenString = startPos;
		else fenString = game.tags[FEN];
		if (strtofen(&fen, fenString)) {
			printf("playGame() error: strtofen() failed; FEN %s in game number %lu\n", fenString, gNumber);
			playGameResult[ctx->threadNumber] = -1;
			break;
		}
		if (classify && strcmp(game.tags[Variant], "chess 960") != 0 && fen.moveNumber == 1 && fen.sideToMove == ColorWhite) {
			ecoClassify(&game, ecoLines, numberOfEcoLines);
			//printf("ECO %s\nOpening %s\nVariation %s\n", game.tags[ECO], game.tags[Opening], game.tags[Variation]);
		}

		if (pgnGame(&game, false)) {
			printf("playGame() error: pgnGame() returned non-zero result in game number %lu\n", gNumber);
			playGameResult[ctx->threadNumber] = -1;
			break;
		}
	}
	fclose(file);
	return &(playGameResult[ctx->threadNumber]);
}

///<summary>
/// plays multiple pgn games from a given pgn file
/// second arguments is eco file name for ECO classification
/// returns the number of games or -1 in case of an error
///</summary>
unsigned long pgnGames(char * fileName, char * ecoFileName) {
	int err = 0;
	FILE * file = fopen(fileName, "r");
	if (!file) {
		printf("countGames() error: failed to open a file %s, %s\n", fileName, strerror(errno));
		return 0;
	}
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
					return 03;
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

	//Multi-threading implementation, where each thread will process one game at a time
	//locking, updating and unlocking the current game number

	pthread_t thread_id[MAX_NUMBER_OF_THREADS];
	for (unsigned int t = 0; t < MAX_NUMBER_OF_THREADS; t++) {
		pthread_attr_t attr;
		if (err = pthread_attr_init(&attr) != 0)
			printf("libchess pgnGames: pthread_attr_init() returned %d\n", err);
		else {
			playGameCtx[t] = malloc(sizeof(struct PlayGameContext));
			if (playGameCtx[t]) {
				strncpy(playGameCtx[t]->fileName, fileName, 255);
				playGameCtx[t]->threadNumber = t;
				if (err = pthread_create(&(thread_id[t]), &attr, &playGame, (void *)(playGameCtx[t])) != 0)
					printf("libchess pgnGames: pthread_create() returned %d\n", err);
			}
			if (err = pthread_attr_destroy(&attr) != 0)
				printf("libchess pgnGames: pthread_attr_destroy() returned %d\n", err);
		}
	}
	for (unsigned int t = 0; t < MAX_NUMBER_OF_THREADS; t++) {
		pthread_join(thread_id[t], NULL);
		free(playGameCtx[t]);
	}
	for (int i = 0; i < numberOfEcoLines; i++) free(ecoLines[i]);
	if (err) return err;
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
