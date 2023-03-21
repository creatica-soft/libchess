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
#include "libchess.h"

//#define READ_END 0
//#define WRITE_END 1

FILE * toEngine, * fromEngine;

//int getOptions(char * engineName, struct Engine * chessEngine) {
int getOptions(struct Engine * chessEngine) {
	char name[MAX_UCI_OPTION_NAME_LEN], type[MAX_UCI_OPTION_TYPE_LEN],
		defaultStringValue[MAX_UCI_OPTION_STRING_LEN];
	long defaultSpinValue, minValue, maxValue;
	/*

	int toEngine[2], fromEngine[2];
	pid_t enginePid;

	if (pipe(toEngine)) {
		printf("engine() error: pipe(toEngine) returned non-zero code\n");
		return 1;
	}
	if (pipe(fromEngine)) {
		printf("engine() error: pipe(fromEngine) returned non-zero code\n");
		return 1;
	}
	enginePid = fork();
	if (enginePid < 0) {
		printf("engine() error: fork failed\n");
		return 1;
	}
	if (enginePid == 0) { //child
		close(toEngine[WRITE_END]);
		close(fromEngine[READ_END]);
		dup2(toEngine[READ_END], STDIN_FILENO);
		close(toEngine[READ_END]);
		dup2(fromEngine[WRITE_END], STDOUT_FILENO);
		close(fromEngine[WRITE_END]);
		if (execlp(engineName, engineName, (char *)NULL) < 0) {
			printf("engine() error: execlp failed\n");
			return 1;
		}
		//_exit(EXIT_SUCCESS);
	}
	else { //parent
		close(toEngine[READ_END]);
		close(fromEngine[WRITE_END]);
		dup2(toEngine[WRITE_END], STDOUT_FILENO);
		close(toEngine[WRITE_END]);
		dup2(fromEngine[READ_END], STDIN_FILENO);
		close(fromEngine[READ_END]);
	}
	*/

	fprintf(toEngine, "uci\n");
	fflush(toEngine);
	fprintf(stderr, "uci\n");
	char line[256];
	chessEngine->numberOfCheckOptions = 0;
	chessEngine->numberOfComboOptions = 0;
	chessEngine->numberOfSpinOptions = 0;
	chessEngine->numberOfStringOptions = 0;
	chessEngine->numberOfButtonOptions = 0;
	char * lineMod = NULL, * tmp = NULL;
	while (fgets(line, sizeof(line), fromEngine)) {
		fprintf(stderr, "%s", line);
		if (strcmp(line, "uciok\n") == 0) break;
		if (strstr(line, "option name ") - line == 0) {
			tmp = line + 12;
			if (lineMod = strstr(tmp, " type ")) {
				size_t s = lineMod - tmp;
				strncpy(name, tmp, s);
				name[s] = '\0';
				lineMod += 6;
				int res = sscanf(lineMod, "%7s", type);
				if (res == 1) {
					//fprintf(stderr, "type %s\n", type);
					for (int i = 0; i < MAX_UCI_OPTION_TYPE_NUM; i++) {
						if (strcmp(optionTypes[i], type) == 0) {
							switch (i) {
							case 0: //button
								if (chessEngine->numberOfButtonOptions < MAX_UCI_OPTION_BUTTON_NUM)
									strcpy(chessEngine->optionButton[chessEngine->numberOfButtonOptions++].name, name);
								else {
									fprintf(stderr, "getOptions() error: number of button options exceeded the maximum of %d\n", MAX_UCI_OPTION_BUTTON_NUM);
									return 1;
								}
								break;
							case 1: //check
								res = sscanf(lineMod, "check default %5s\n", defaultStringValue);
								if (res < 1) {
									fprintf(stderr, "getOptions() error: unable to parse the check option line '%s'", line);
									return 1;
								}
								if (chessEngine->numberOfCheckOptions < MAX_UCI_OPTION_CHECK_NUM) {
									strcpy(chessEngine->optionCheck[chessEngine->numberOfCheckOptions].name, name);
									if (strcmp(defaultStringValue, "false") == 0) {
										chessEngine->optionCheck[chessEngine->numberOfCheckOptions].defaultValue = false;
										chessEngine->optionCheck[chessEngine->numberOfCheckOptions++].value = false;
									}
									else if (strcmp(defaultStringValue, "true") == 0) {
										chessEngine->optionCheck[chessEngine->numberOfCheckOptions].defaultValue = true;
										chessEngine->optionCheck[chessEngine->numberOfCheckOptions++].value = true;
									}
								} else {
									fprintf(stderr, "getOptions() error: number of check options exceeded the maximum of %d\n", MAX_UCI_OPTION_CHECK_NUM);
									return 1;
								}
								break;
							case 2: //combo
								res = sscanf(lineMod, "combo default %31s", defaultStringValue);
								if (res < 1) {
									fprintf(stderr, "getOptions() error: unable to parse the combo option line '%s'", line);
									return 1;
								}
								if (chessEngine->numberOfComboOptions < MAX_UCI_OPTION_COMBO_NUM) {
									strcpy(chessEngine->optionCombo[chessEngine->numberOfComboOptions].name, name);
									strcpy(chessEngine->optionCombo[chessEngine->numberOfComboOptions].defaultValue, defaultStringValue);
									memset(chessEngine->optionCombo[chessEngine->numberOfComboOptions].values, 0, sizeof(char[MAX_UCI_OPTION_COMBO_VARS][MAX_UCI_OPTION_STRING_LEN]));
									char * var;
									int n = 0;
									var = line;
									while (var = strstr(var, " var ")) {
										res = sscanf(var, " var %31s", defaultStringValue);
										if (res == 1 && n < MAX_UCI_OPTION_COMBO_VARS)
											strcpy(chessEngine->optionCombo[chessEngine->numberOfComboOptions].values[n++], defaultStringValue);
										var += 5;
									}
									chessEngine->numberOfComboOptions++;
								} else {
									fprintf(stderr, "getOptions() error: number of combo options exceeded the maximum of %d\n", MAX_UCI_OPTION_COMBO_NUM);
									return 1;
								}
								break;
							case 3: //spin
								res = sscanf(lineMod, "spin default %ld min %ld max %ld\n", &defaultSpinValue, &minValue, &maxValue);
								if (res < 3) {
									fprintf(stderr, "getOptions() error: unable to parse the spin option line '%s'", line);
									return 1;
								}
								if (chessEngine->numberOfSpinOptions < MAX_UCI_OPTION_SPIN_NUM) {
									strcpy(chessEngine->optionSpin[chessEngine->numberOfSpinOptions].name, name);
									chessEngine->optionSpin[chessEngine->numberOfSpinOptions].defaultValue = defaultSpinValue;
									chessEngine->optionSpin[chessEngine->numberOfSpinOptions].value = defaultSpinValue;
									chessEngine->optionSpin[chessEngine->numberOfSpinOptions].min = minValue;
									chessEngine->optionSpin[chessEngine->numberOfSpinOptions++].max = maxValue;
								} else {
									fprintf(stderr, "getOptions() error: number of spin options exceeded the maximum of %d\n", MAX_UCI_OPTION_SPIN_NUM);
									return 1;
								}
								break;
							case 4: //string
								res = sscanf(lineMod, "string default %31s\n", defaultStringValue);
								if (res < 1) {
									//fprintf(stderr, "getOptions() warning: unable to parse the string option line '%s'", line);
									defaultStringValue[0] = '\0';
								}
								if (chessEngine->numberOfStringOptions < MAX_UCI_OPTION_STRING_NUM) {
									strcpy(chessEngine->optionString[chessEngine->numberOfStringOptions].name, name);
									strcpy(chessEngine->optionString[chessEngine->numberOfStringOptions++].defaultValue, defaultStringValue);
									strcpy(chessEngine->optionString[chessEngine->numberOfStringOptions++].value, defaultStringValue);
								} else {
									fprintf(stderr, "getOptions() error: number of string options exceeded the maximum of %d\n", MAX_UCI_OPTION_STRING_NUM);
									return 1;
								}
								break;
							}
							break;
						}
					}
				}
			}
		}
		else if (strstr(line, "id name ") - line == 0) {
			strncpy(chessEngine->id, line + 8, MAX_UCI_OPTION_STRING_LEN);
			chessEngine->id[strlen(chessEngine->id) - 1] = '\0';
		}
		else if (strstr(line, "id author ") - line == 0) {
			strncpy(chessEngine->authors, line + 10, 2 * MAX_UCI_OPTION_STRING_LEN);
			chessEngine->authors[strlen(chessEngine->authors) - 1] = '\0';
		}
	}

/*
	fprintf(stderr, "id %s\n", chessEngine->id);
	fprintf(stderr, "authors %s\n", chessEngine->authors);
	for (int i = 0; i < chessEngine->numberOfButtonOptions; i++)
		fprintf(stderr, "option name %s type button\n", chessEngine->optionButton[i].name);
	for (int i = 0; i < chessEngine->numberOfCheckOptions; i++)
		fprintf(stderr, "option name %s type check default %s\n", chessEngine->optionCheck[i].name, chessEngine->optionCheck[i].defaultValue == 1 ? "true" : "false");
	for (int i = 0; i < chessEngine->numberOfComboOptions; i++) {
		char vars[256];
		vars[0] = '\0';
		for (int j = 0; j < MAX_UCI_OPTION_COMBO_VARS; j++) {
			if (chessEngine->optionCombo[i].values[j][0] != '\0') {
				strcat(vars, " var ");
				strcat(vars, chessEngine->optionCombo[i].values[j]);
			}
		}
		fprintf(stderr, "option name %s type combo default %s%s\n", chessEngine->optionCombo[i].name, chessEngine->optionCombo[i].defaultValue, vars);
	}
	for (int i = 0; i < chessEngine->numberOfSpinOptions; i++)
		fprintf(stderr, "option name %s type spin default %ld min %ld max %ld\n", chessEngine->optionSpin[i].name, chessEngine->optionSpin[i].defaultValue, chessEngine->optionSpin[i].min, chessEngine->optionSpin[i].max);
	for (int i = 0; i < chessEngine->numberOfStringOptions; i++)
		fprintf(stderr, "option name %s type string default %s\n", chessEngine->optionString[i].name, chessEngine->optionString[i].defaultValue);
*/
	return 0;
}

int nametoindex(struct Engine * chessEngine, char * name, enum OptionType type) {
	int idx = -1;
	switch (type) {
	case 0: //button
		for (int i = 0; i < chessEngine->numberOfButtonOptions; i++) {
			if (strncmp(chessEngine->optionButton[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case 1: //check
		for (int i = 0; i < chessEngine->numberOfCheckOptions; i++) {
			if (strncmp(chessEngine->optionCheck[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case 2: //combo
		for (int i = 0; i < chessEngine->numberOfComboOptions; i++) {
			if (strncmp(chessEngine->optionCombo[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case 3: //spin
		for (int i = 0; i < chessEngine->numberOfSpinOptions; i++) {
			if (strncmp(chessEngine->optionSpin[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case 4: //string
		for (int i = 0; i < chessEngine->numberOfStringOptions; i++) {
			if (strncmp(chessEngine->optionString[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	}
	if (idx < 0) {
		fprintf(stderr, "nametoindex() warning: engine %s does not know option %s of type %s\n", chessEngine->id, name, optionTypes[type]);
	}
	return idx;
}

int setOption(struct Engine * chessEngine, char * name, enum OptionType type, void * value) {
	char line[256];
	line[0] = '\0';
	long v;
	int idx;
	bool val;

	switch (type) {
	case 0: //button
		idx = nametoindex(chessEngine, name, Button);
		if (idx < 0) return 1;
		sprintf(line, "setoption name %s", name);
		break;
	case 1: //check
		idx = nametoindex(chessEngine, name, Check);
		if (idx < 0) return 1;
		val = *(bool *)value;
		sprintf(line, "setoption name %s value %s", name, val ? "true" : "false");
		break;
	case 2: //combo
		idx = nametoindex(chessEngine, name, Combo);
		if (idx < 0) return 1;
		sprintf(line, "setoption name %s value %s", name, (char *)value);
		break;
	case 3: //spin
		v = *(long *)value;
		idx = nametoindex(chessEngine, name, Spin);
		if (idx < 0) return 1;
		if (v <= chessEngine->optionSpin[idx].max && v >= chessEngine->optionSpin[idx].min)
			sprintf(line, "setoption name %s value %ld", name, v);
		else {
			fprintf(stderr, "setOption() failed: spin option %s value (%ld) is outside min-max range: %ld - %ld\n", name, v, chessEngine->optionSpin[idx].min, chessEngine->optionSpin[idx].max);
			return 1;
		}
		break;
	case 4: //string
		idx = nametoindex(chessEngine, name, String);
		if (idx < 0) return 1;
		sprintf(line, "setoption name %s value %s", name, (char *)value);
		break;
	}
	fprintf(toEngine, "%s\n", line);
	fflush(toEngine);
	fprintf(stderr, "%s\n", line);
	// without any output from the engine, fgets() will block
	//while (fgets(line, sizeof(line), stdin)) {
	//	fprintf(stderr, "%s", line);
	//}
	return 0;
}

void setOptions(struct Engine * chessEngine) {
	for (int i = 0; i < chessEngine->numberOfButtonOptions; i++) {
		if (chessEngine->optionButton[i].value)
			if (setOption(chessEngine, chessEngine->optionButton[i].name, Button, NULL))
				fprintf(stderr, "engine() warning: setOption('%s') returned non-zero code\n", chessEngine->optionButton[i].name);
	}
	for (int i = 0; i < chessEngine->numberOfCheckOptions; i++) {
		if (chessEngine->optionCheck[i].value != chessEngine->optionCheck[i].defaultValue)
			if (setOption(chessEngine, chessEngine->optionCheck[i].name, Check, &(chessEngine->optionCheck[i].value)))
				fprintf(stderr, "engine() failed: setOption('%s', %s) returned non-zero code\n", chessEngine->optionCheck[i].name, chessEngine->optionCheck[i].value ? "true" : "false");
	}
	for (int i = 0; i < chessEngine->numberOfComboOptions; i++) {
		if (chessEngine->optionCombo[i].value != chessEngine->optionCombo[i].defaultValue) {
			for (int j = 0; j < MAX_UCI_OPTION_COMBO_VARS; j++) {
				if (strcmp(chessEngine->optionCombo[i].value, chessEngine->optionCombo[i].values[j]) == 0) {
					if (setOption(chessEngine, chessEngine->optionCombo[i].name, Combo, &(chessEngine->optionCombo[i].value)))
						fprintf(stderr, "engine() failed: setOption('%s', %s) returned non-zero code\n", chessEngine->optionCombo[i].name, chessEngine->optionCombo[i].value);
					break;
				}
				if (j == MAX_UCI_OPTION_COMBO_VARS) {
					fprintf(stderr, "Combo option %s has no such value %s. Allowed values are: ", chessEngine->optionCombo[i].name, chessEngine->optionCombo[i].value);
					for (j = 0; j < MAX_UCI_OPTION_COMBO_VARS; j++)
						fprintf(stderr, " '%s'", chessEngine->optionCombo[i].values[j]);
					fprintf(stderr, "\n");
				}
			}
		}
	}
	for (int i = 0; i < chessEngine->numberOfSpinOptions; i++) {
		if (chessEngine->optionSpin[i].value != chessEngine->optionSpin[i].defaultValue)
			if (setOption(chessEngine, chessEngine->optionSpin[i].name, Spin, &(chessEngine->optionSpin[i].value)))
				fprintf(stderr, "engine() failed: setOption(%s, %ld) returned non-zero code\n", chessEngine->optionSpin[i].name, chessEngine->optionSpin[i].value);
	}
	for (int i = 0; i < chessEngine->numberOfStringOptions; i++) {
		if (strcmp(chessEngine->optionString[i].value, chessEngine->optionString[i].defaultValue) != 0)
			if (setOption(chessEngine, "Debug Log File", String, &(chessEngine->optionString[i].value)))
				fprintf(stderr, "engine() failed: setOption('%s', '%s') returned non-zero code\n", chessEngine->optionString[i].name, chessEngine->optionString[i].value);
	}
}

bool isReady() {
	bool ready = false;
	char line[256];
	fprintf(toEngine, "isready\n");
	fflush(toEngine);
	fprintf(stderr, "isready\n");
	while (fgets(line, sizeof(line), fromEngine)) {
		fprintf(stderr, "%s", line);
		if (strcmp(line, "readyok\n") == 0) {
			ready = true;
			break;
		}
	}
	return ready;
}

bool newGame() {
	fprintf(toEngine, "ucinewgame\n");
	fflush(toEngine);
	fprintf(stderr, "ucinewgame\n");
	return isReady();
}

void stop() {
	fprintf(toEngine, "stop\n");
	fflush(toEngine);
	fprintf(stderr, "stop\n");
}

void quit() {
	fprintf(toEngine, "quit\n");
	fflush(toEngine);
	fprintf(stderr, "quit\n");
}

bool position(char * fen, char * moves) {
	char line[4096];
	if (fen)
		if (strlen(fen))
			sprintf(line, "position fen %s", fen);
	else sprintf(line, "position startpos");
	if (moves) {
		strcat(line, " moves ");
		strncat(line, moves, sizeof line - strlen(line));
	}
	fprintf(toEngine, "%s\n", line);
	fflush(toEngine);
	fprintf(stderr, "%s\n", line);
	return isReady();
}

int getPV(struct Evaluation ** eval, int multiPV) {
	char line[2048];
	char * prevLine[MAX_UCI_MULTI_PV];
	if (multiPV > MAX_UCI_MULTI_PV) {
		fprintf(stderr, "getPV() error: multiPV is greater than the maximum of %d\n", MAX_UCI_MULTI_PV);
		return 1;
	}
	for (int i = 0; i < multiPV; i++) prevLine[i] = NULL;
	int pv;
	char * tmpLine;
	size_t len, prevLen[MAX_UCI_MULTI_PV] = { 0 };
	while (fgets(line, sizeof(line), fromEngine)) {
		//fprintf(stderr, "%s", line);
		if (strstr(line, "bestmove ") - line == 0) {
			if (sscanf(line, "bestmove %5s ponder %5s\n", eval[0]->bestmove, eval[0]->ponder) == 2) {
				for (int i = 0; i < multiPV; i++) {
					sscanf(prevLine[i], "info depth %hhu seldepth %hhu multipv %hhu score cp %d nodes %lu nps %lu hashfull %hu tbhits %hhu time %lu pv %1024[abcdefghnqr12345678\040]\n", &(eval[i]->depth), &(eval[i]->seldepth), &(eval[i]->multipv), &(eval[i]->scorecp), &(eval[i]->nodes), &(eval[i]->nps), &(eval[i]->hashful), &(eval[i]->tbhits), &(eval[i]->time), eval[i]->pv);
					tmpLine = strstr(prevLine[i], " mate ");
					if (tmpLine) sscanf(tmpLine + 6, "%d", &(eval[0]->matein));
					free(prevLine[i]);
				}
				//White has a slight advantage
				if (eval[0]->scorecp > INACCURACY && eval[0]->scorecp <= MISTAKE)
					eval[0]->nag = 14;
				//White has a moderate advantage
				else if (eval[0]->scorecp > MISTAKE && eval[0]->scorecp <= BLUNDER)
					eval[0]->nag = 16;
				//White has a decisive advantage
				else if (eval[0]->scorecp > BLUNDER && eval[0]->scorecp < MATE_SCORE / 2)
					eval[0]->nag = 18;
				//Black has a slight advantage
				else if (eval[0]->scorecp < -INACCURACY && eval[0]->scorecp >= -MISTAKE)
					eval[0]->nag = 15;
				//Black has a moderate advantage
				else if (eval[0]->scorecp < -MISTAKE && eval[0]->scorecp >= -BLUNDER)
					eval[0]->nag = 17;
				//Black has a decisive advantage
				else if (eval[0]->scorecp < -BLUNDER && eval[0]->scorecp > -MATE_SCORE / 2)
					eval[0]->nag = 19;
				//White has a crushing advantage (Black should resign)
				else if (eval[0]->scorecp >= MATE_SCORE / 2 && eval[0]->scorecp < MATE_SCORE) {
					if (abs(eval[0]->matein) > MAX_VARIATION_PLIES) eval[0]->nag = 20;
				}
				//Black has a crushing advantage (White should resign)
				else if (eval[0]->scorecp <= -MATE_SCORE / 2 && eval[0]->scorecp > -MATE_SCORE) {
					if (abs(eval[0]->matein) > MAX_VARIATION_PLIES) eval[0]->nag = 21;
				}
				//Drawish position
				else eval[0]->nag = 10;
				return 0;
			}
		}
		else {
			if (tmpLine = strstr(line, " multipv ")) {
				if (sscanf(tmpLine + 9, "%d ", &pv)) {
					if (tmpLine = strstr(tmpLine, " pv ")) {
						tmpLine += 4;
						len = strlen(tmpLine) / 5; // this will give ply number (more or less)
						pv--; // PV numbers are 1-based, arrays are 0-based
						// save the longest PV, which is not greater than maxPlies
						if (len <= eval[pv]->maxPlies && len >= prevLen[pv]) {
							prevLen[pv] = len;
							if (prevLine[pv]) free(prevLine[pv]);
							prevLine[pv] = malloc(strlen(line) + 1);
							strcpy(prevLine[pv], line);
						}
					}
				}
			}
		}
	}
}

/*
* searchmoves <move1> .... <movei>
	restrict search to this moves only
	Example: After "position startpos" and "go infinite searchmoves e2e4 d2d4"
	the engine should only search the two moves e2e4 and d2d4 in the initial 
	position.
* ponder
	start searching in pondering mode.
	Do not exit the search in ponder mode, even if it's mate!
	This means that the last move sent in in the position string is the ponder move.
	The engine can do what it wants to do, but after a "ponderhit" command
	it should execute the suggested move to ponder on. This means that the ponder move sent by
	the GUI can be interpreted as a recommendation about which move to ponder. However, if the
	engine decides to ponder on a different move, it should not display any mainlines as they are
	likely to be misinterpreted by the GUI because the GUI expects the engine to ponder
	on the suggested move.
* wtime <x>
	white has x msec left on the clock
* btime <x>
	black has x msec left on the clock
* winc <x>
	white increment per move in mseconds if x > 0
* binc <x>
	black increment per move in mseconds if x > 0
* movestogo <x>
	there are x moves to the next time control,
	this will only be sent if x > 0,
	if you don't get this and get the wtime and btime it's sudden death
* depth <x>
	search x plies only.
* nodes <x>
	search x nodes only,
* mate <x>
	search for a mate in x moves
* movetime <x>
	search exactly x mseconds
* infinite
	search until the "stop" command. Do not exit the search without being told so in this mode!
*/
//void go(long movetime, int depth, int nodes, int mate, char * searchmoves, bool ponder, bool infinite, long wtime, long btime, long winc, long binc, int movestogo, struct Engine * chessEngine, struct Evaluation ** eval) {
void go(struct Engine * chessEngine, struct Evaluation ** eval) {
	char line[4096], tmp[256];
	sprintf(line, "go");
	if (chessEngine->movetime) {
		sprintf(tmp, " movetime %ld", chessEngine->movetime);
		strcat(line, tmp);
	}
	if (chessEngine->depth) {
		sprintf(tmp, " depth %d", chessEngine->depth);
		strcat(line, tmp);
	}
	if (chessEngine->nodes) {
		sprintf(tmp, " nodes %d", chessEngine->nodes);
		strcat(line, tmp);
	}
	if (chessEngine->mate) {
		sprintf(tmp, " mate %d", chessEngine->mate);
		strcat(line, tmp);
	}
	if (chessEngine->searchmoves) {
		sprintf(tmp, " searchmoves %s", chessEngine->searchmoves);
		strcat(line, tmp);
	}
	if (chessEngine->wtime) {
		sprintf(tmp, " wtime %ld", chessEngine->wtime);
		strcat(line, tmp);
	}
	if (chessEngine->btime) {
		sprintf(tmp, " btime %ld", chessEngine->btime);
		strcat(line, tmp);
	}
	if (chessEngine->winc) {
		sprintf(tmp, " winc %ld", chessEngine->winc);
		strcat(line, tmp);
	}
	if (chessEngine->binc) {
		sprintf(tmp, " binc %ld", chessEngine->binc);
		strcat(line, tmp);
	}
	if (chessEngine->movestogo) {
		sprintf(tmp, " movestogo %d", chessEngine->movestogo);
		strcat(line, tmp);
	}
	if (chessEngine->ponder) strcat(line, " chessEngine->ponder");
	if (chessEngine->infinite) strcat(line, " chessEngine->infinite");

	fprintf(toEngine, "%s\n", line);
	fflush(toEngine);
	fprintf(stderr, "%s\n", line);

	int multiPV = nametoindex(chessEngine, "MultiPV", Spin);
	if (multiPV < 0) {
		fprintf(stderr, "engine() failed: nametoindex(MultiPV, Spin) return -1\n");
		return;
	}
	//fprintf(stderr, "multiPV %ld\n", chessEngine->optionSpin[multiPV].value);
	getPV(eval, chessEngine->optionSpin[multiPV].value);
}


//int engine(char * engineName, struct Engine * chessEngine, struct Evaluation ** eval) {
int engine(char * engineName) {
	//int toEngine[2], fromEngine[2];
	pid_t enginePid;
	unlink(TO_ENGINE_NAMED_PIPE);
	unlink(FROM_ENGINE_NAMED_PIPE);
	//pipe is only useful between parent and child processes
	//here it is better to use named pipes, i.e. fifo
	//if (pipe(toEngine)) {
	if (mkfifo(TO_ENGINE_NAMED_PIPE, S_IRUSR | S_IWUSR) == -1) {
		printf("engine() error: mkfifo(%s, S_IRUSR | S_IWUSR) returned %s\n", TO_ENGINE_NAMED_PIPE, strerror(errno));
		return 1;
	}
	//if (pipe(fromEngine)) {
	if (mkfifo(FROM_ENGINE_NAMED_PIPE, S_IRUSR | S_IWUSR) == -1) {
		printf("engine() error: mkfifo(%s, S_IRUSR | S_IWUSR) returned %s\n", FROM_ENGINE_NAMED_PIPE, strerror(errno));
		return 1;
	}
	enginePid = fork();
	if (enginePid < 0) {
		printf("engine() error: fork failed\n");
		return 1;
	}
	if (enginePid == 0) { //child
		//close(toEngine[WRITE_END]);
		//close(fromEngine[READ_END]);
		//dup2(toEngine[READ_END], STDIN_FILENO);
		//close(toEngine[READ_END]);
		//dup2(fromEngine[WRITE_END], STDOUT_FILENO);
		//close(fromEngine[WRITE_END]);
		int toEngine, fromEngine;
		if ((toEngine = open(TO_ENGINE_NAMED_PIPE, O_RDONLY)) == -1) {
			printf("engine() child error: open(%s, O_RDONLY) returned %s\n", TO_ENGINE_NAMED_PIPE, strerror(errno));
			return 1;
		}
		printf("engine() child: open(%s, O_RDONLY) ok\n", TO_ENGINE_NAMED_PIPE);
		if ((fromEngine = open(FROM_ENGINE_NAMED_PIPE, O_WRONLY)) == -1) {
			printf("engine() child error: open(%s, O_WRONLY) returned %s\n", FROM_ENGINE_NAMED_PIPE, strerror(errno));
			close(toEngine);
			return 1;
		}
		printf("engine() child: open(%s, O_WRONLY) ok\n", FROM_ENGINE_NAMED_PIPE);
		dup2(toEngine, STDIN_FILENO);
		close(toEngine);
		dup2(fromEngine, STDOUT_FILENO);
		close(fromEngine);
		if (execlp((char *)engineName, (char *)engineName, (char *)NULL) < 0) {
			printf("engine() child error: execlp failed\n");
			return 1;
		}
	}
	else { //parent
		/*
		close(toEngine[READ_END]);
		close(fromEngine[WRITE_END]);
		dup2(toEngine[WRITE_END], STDOUT_FILENO);
		close(toEngine[WRITE_END]);
		dup2(fromEngine[READ_END], STDIN_FILENO);
		close(fromEngine[READ_END]);
		*/

		if ((toEngine = fopen(TO_ENGINE_NAMED_PIPE, "w")) == NULL) {
			printf("engine() parent error: fopen(%s, w) returned %s\n", TO_ENGINE_NAMED_PIPE, strerror(errno));
			return 1;
		}
		printf("engine() parent: fopen(%s, w) ok\n", TO_ENGINE_NAMED_PIPE);
		if ((fromEngine = fopen(FROM_ENGINE_NAMED_PIPE, "r")) == NULL) {
			printf("engine() parent error: fopen(%s, r) returned %s\n", FROM_ENGINE_NAMED_PIPE, strerror(errno));
			fclose(toEngine);
			return 1;
		}
		printf("engine() parent: fopen(%s, r) ok\n", FROM_ENGINE_NAMED_PIPE);
		//dup2(toEngine, STDOUT_FILENO);
		//close(toEngine);
		//dup2(fromEngine, STDIN_FILENO);
		//close(fromEngine);
		//dup2(toEngine, STDOUT_FILENO);
		//dup2(fromEngine, STDIN_FILENO);

		/*
		for (int i = 0; i < chessEngine->numberOfButtonOptions; i++) {
			if (chessEngine->optionButton[i].value)
				if (setOption(chessEngine, chessEngine->optionButton[i].name, Button, NULL))
					fprintf(stderr, "engine() warning: setOption('%s') returned non-zero code\n", chessEngine->optionButton[i].name);
		}
		for (int i = 0; i < chessEngine->numberOfCheckOptions; i++) {
			if (chessEngine->optionCheck[i].value != chessEngine->optionCheck[i].defaultValue)
				if (setOption(chessEngine, chessEngine->optionCheck[i].name, Check, &(chessEngine->optionCheck[i].value)))
					fprintf(stderr, "engine() failed: setOption('%s', %s) returned non-zero code\n", chessEngine->optionCheck[i].name, chessEngine->optionCheck[i].value ? "true" : "false");
		}
		for (int i = 0; i < chessEngine->numberOfComboOptions; i++) {
			if (chessEngine->optionCombo[i].value != chessEngine->optionCombo[i].defaultValue) {
				for (int j = 0; j < MAX_UCI_OPTION_COMBO_VARS; j++) {
					if (strcmp(chessEngine->optionCombo[i].value, chessEngine->optionCombo[i].values[j]) == 0) {
						if (setOption(chessEngine, chessEngine->optionCombo[i].name, Combo, &(chessEngine->optionCombo[i].value)))
							fprintf(stderr, "engine() failed: setOption('%s', %s) returned non-zero code\n", chessEngine->optionCombo[i].name, chessEngine->optionCombo[i].value);
						break;
					}
					if (j == MAX_UCI_OPTION_COMBO_VARS) {
						fprintf(stderr, "Combo option %s has no such value %s. Allowed values are: ", chessEngine->optionCombo[i].name, chessEngine->optionCombo[i].value);
						for (j = 0; j < MAX_UCI_OPTION_COMBO_VARS; j++)
							fprintf(stderr, " '%s'", chessEngine->optionCombo[i].values[j]);
						fprintf(stderr, "\n");
					}
				}
			}
		}
		for (int i = 0; i < chessEngine->numberOfSpinOptions; i++) {
			if (chessEngine->optionSpin[i].value != chessEngine->optionSpin[i].defaultValue)
				if (setOption(chessEngine, chessEngine->optionSpin[i].name, Spin, &(chessEngine->optionSpin[i].value)))
					fprintf(stderr, "engine() failed: setOption(%s, %ld) returned non-zero code\n", chessEngine->optionSpin[i].name, chessEngine->optionSpin[i].value);
		}
		for (int i = 0; i < chessEngine->numberOfStringOptions; i++) {
			if (strcmp(chessEngine->optionString[i].value, chessEngine->optionString[i].defaultValue) != 0)
				if (setOption(chessEngine, "Debug Log File", String, &(chessEngine->optionString[i].value)))
					fprintf(stderr, "engine() failed: setOption('%s', '%s') returned non-zero code\n", chessEngine->optionString[i].name, chessEngine->optionString[i].value);
		}
		fprintf(stderr, "position %s moves %s\n", chessEngine->position, chessEngine->moves);
		if (!isReady()) {
			fprintf(stderr, "engine() error: chess engine %s is not ready\n", chessEngine->id);
			return 1;
		}
		if (!newGame()) {
			fprintf(stderr, "engine() newGame() error: chess engine %s is not ready\n", chessEngine->id);
			return 1;
		}
		if (!position(chessEngine->position, chessEngine->moves)) {
			fprintf(stderr, "engine() position() error: chess engine %s is not ready\n", chessEngine->id);
			return 1;
		}
		go(chessEngine->movetime, chessEngine->depth, chessEngine->nodes, chessEngine->mate, chessEngine->searchmoves, chessEngine->ponder, chessEngine->infinite, chessEngine->wtime, chessEngine->btime, chessEngine->winc, chessEngine->binc, chessEngine->movestogo);
		int multiPV = nametoindex(chessEngine, "MultiPV", Spin);
		if (multiPV < 0) {
			fprintf(stderr, "engine() failed: nametoindex(MultiPV, Spin) return -1\n");
			return 1;
		}
		//fprintf(stderr, "multiPV %ld\n", chessEngine->optionSpin[multiPV].value);
		getPV(eval, chessEngine->optionSpin[multiPV].value);
		*/
		//for (int i = chessEngine->optionSpin[multiPV].value - 1; i >= 0;  i--) {
		//	fprintf(stderr, "info depth %hhu seldepth %hhu multipv %hhu score cp %d nodes %lu nps %lu hashfull %hu tbhits %hhu time %lu pv %s\n", eval[i]->depth, eval[i]->seldepth, eval[i]->multipv, eval[i]->scorecp, eval[i]->nodes, eval[i]->nps, eval[i]->hashful, eval[i]->tbhits, eval[i]->time, eval[i]->pv);
		//	if (i == 0)
		//		fprintf(stderr, "bestmove %s ponder %s\n", eval[0]->bestmove, eval[0]->ponder);
		//}

		//fprintf(stdout, "quit\n");
		//fflush(stdout);
		//close(toEngine);
		//fflush(stdin);
		//close(fromEngine);
		//wait(NULL); //wait for the child to exit - blocks!
		//fclose(toEngine);
		//fclose(fromEngine);
	}
	return 0;
}