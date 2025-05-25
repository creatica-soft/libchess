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
#include <math.h>
#include "libchess.h"

int getOptions(struct Engine * engine) {
	char name[MAX_UCI_OPTION_NAME_LEN], type[MAX_UCI_OPTION_TYPE_LEN],
		defaultStringValue[MAX_UCI_OPTION_STRING_LEN];
	long defaultSpinValue, minValue, maxValue;

	fprintf(engine->toEngine, "uci\n");
	fflush(engine->toEngine);
	//fprintf(stderr, "uci\n");
	char line[256];
	engine->numberOfCheckOptions = 0;
	engine->numberOfComboOptions = 0;
	engine->numberOfSpinOptions = 0;
	engine->numberOfStringOptions = 0;
	engine->numberOfButtonOptions = 0;
	char * lineMod = NULL, * tmp = NULL;
	while (fgets(line, sizeof(line), engine->fromEngine)) {
		//fprintf(stderr, "%s", line);
		if (strcmp(line, "uciok\n") == 0) break;
		if (strstr(line, "option name ") - line == 0) {
			tmp = line + 12;
			if ((lineMod = strstr(tmp, " type "))) {
				size_t s = lineMod - tmp;
				strncpy(name, tmp, s);
				name[s] = '\0';
				lineMod += 6;
				int res = sscanf(lineMod, "%7s", type);
				if (res == 1) {
					for (int i = 0; i < MAX_UCI_OPTION_TYPE_NUM; i++) {
						if (strcmp(optionTypes[i], type) == 0) {
							switch (i) {
							case 0: //button
								if (engine->numberOfButtonOptions < MAX_UCI_OPTION_BUTTON_NUM)
									strcpy(engine->optionButton[engine->numberOfButtonOptions++].name, name);
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
								if (engine->numberOfCheckOptions < MAX_UCI_OPTION_CHECK_NUM) {
									strcpy(engine->optionCheck[engine->numberOfCheckOptions].name, name);
									if (strcmp(defaultStringValue, "false") == 0) {
										engine->optionCheck[engine->numberOfCheckOptions].defaultValue = false;
										engine->optionCheck[engine->numberOfCheckOptions++].value = false;
									}
									else if (strcmp(defaultStringValue, "true") == 0) {
										engine->optionCheck[engine->numberOfCheckOptions].defaultValue = true;
										engine->optionCheck[engine->numberOfCheckOptions++].value = true;
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
								if (engine->numberOfComboOptions < MAX_UCI_OPTION_COMBO_NUM) {
									strcpy(engine->optionCombo[engine->numberOfComboOptions].name, name);
									strcpy(engine->optionCombo[engine->numberOfComboOptions].defaultValue, defaultStringValue);
									memset(engine->optionCombo[engine->numberOfComboOptions].values, 0, sizeof(char[MAX_UCI_OPTION_COMBO_VARS][MAX_UCI_OPTION_STRING_LEN]));
									char * var;
									int n = 0;
									var = line;
									while ((var = strstr(var, " var "))) {
										res = sscanf(var, " var %31s", defaultStringValue);
										if (res == 1 && n < MAX_UCI_OPTION_COMBO_VARS)
											strcpy(engine->optionCombo[engine->numberOfComboOptions].values[n++], defaultStringValue);
										var += 5;
									}
									engine->numberOfComboOptions++;
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
								if (engine->numberOfSpinOptions < MAX_UCI_OPTION_SPIN_NUM) {
									strcpy(engine->optionSpin[engine->numberOfSpinOptions].name, name);
									engine->optionSpin[engine->numberOfSpinOptions].defaultValue = defaultSpinValue;
									engine->optionSpin[engine->numberOfSpinOptions].value = defaultSpinValue;
									engine->optionSpin[engine->numberOfSpinOptions].min = minValue;
									engine->optionSpin[engine->numberOfSpinOptions++].max = maxValue;
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
								if (engine->numberOfStringOptions < MAX_UCI_OPTION_STRING_NUM) {
									strcpy(engine->optionString[engine->numberOfStringOptions].name, name);
									strcpy(engine->optionString[engine->numberOfStringOptions].defaultValue, defaultStringValue);
									strcpy(engine->optionString[engine->numberOfStringOptions++].value, defaultStringValue);
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
			strncpy(engine->id, line + 8, MAX_UCI_OPTION_STRING_LEN);
			engine->id[strlen(engine->id) - 1] = '\0';
		}
		else if (strstr(line, "id author ") - line == 0) {
			strncpy(engine->authors, line + 10, 2 * MAX_UCI_OPTION_STRING_LEN);
			engine->authors[strlen(engine->authors) - 1] = '\0';
		}
	}
	return 0;
}

int nametoindex(struct Engine * engine, char * name, enum OptionType type) {
	int idx = -1;
	switch (type) {
	case 0: //button
		for (int i = 0; i < engine->numberOfButtonOptions; i++) {
			if (strncmp(engine->optionButton[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case 1: //check
		for (int i = 0; i < engine->numberOfCheckOptions; i++) {
			if (strncmp(engine->optionCheck[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case 2: //combo
		for (int i = 0; i < engine->numberOfComboOptions; i++) {
			if (strncmp(engine->optionCombo[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case 3: //spin
		for (int i = 0; i < engine->numberOfSpinOptions; i++) {
			if (strncmp(engine->optionSpin[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case 4: //string
		for (int i = 0; i < engine->numberOfStringOptions; i++) {
			if (strncmp(engine->optionString[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	}
	if (idx < 0) {
		fprintf(stderr, "nametoindex() warning: engine %s does not know option %s of type %s\n", engine->id, name, optionTypes[type]);
	}
	return idx;
}

int setOption(struct Engine * engine, char * name, enum OptionType type, void * value) {
	char line[256];
	line[0] = '\0';
	long v;
	int idx;
	bool val;

	switch (type) {
	case 0: //button
		idx = nametoindex(engine, name, Button);
		if (idx < 0) return 1;
		sprintf(line, "setoption name %s", name);
		break;
	case 1: //check
		idx = nametoindex(engine, name, Check);
		if (idx < 0) return 1;
		val = *(bool *)value;
		sprintf(line, "setoption name %s value %s", name, val ? "true" : "false");
		break;
	case 2: //combo
		idx = nametoindex(engine, name, Combo);
		if (idx < 0) return 1;
		sprintf(line, "setoption name %s value %s", name, (char *)value);
		break;
	case 3: //spin
		v = *(long *)value;
		idx = nametoindex(engine, name, Spin);
		if (idx < 0) return 1;
		if (v <= engine->optionSpin[idx].max && v >= engine->optionSpin[idx].min)
			sprintf(line, "setoption name %s value %ld", name, v);
		else {
			fprintf(stderr, "setOption() failed: spin option %s value (%ld) is outside min-max range: %ld - %ld\n", name, v, engine->optionSpin[idx].min, engine->optionSpin[idx].max);
			return 1;
		}
		break;
	case 4: //string
		idx = nametoindex(engine, name, String);
		if (idx < 0) return 1;
		sprintf(line, "setoption name %s value %s", name, (char *)value);
		break;
	}
	fprintf(engine->toEngine, "%s\n", line);
	fflush(engine->toEngine);
	//fprintf(stderr, "%s\n", line);
	// without any output from the engine, fgets() will block
	//while (fgets(line, sizeof(line), stdin)) {
	//	fprintf(stderr, "%s", line);
	//}
	return 0;
}

void setOptions(struct Engine * engine) {
	for (int i = 0; i < engine->numberOfButtonOptions; i++) {
		if (engine->optionButton[i].value)
			if (setOption(engine, engine->optionButton[i].name, Button, NULL))
				fprintf(stderr, "engine() warning: setOption('%s') returned non-zero code\n", engine->optionButton[i].name);
	}
	for (int i = 0; i < engine->numberOfCheckOptions; i++) {
		if (engine->optionCheck[i].value != engine->optionCheck[i].defaultValue)
			if (setOption(engine, engine->optionCheck[i].name, Check, &(engine->optionCheck[i].value)))
				fprintf(stderr, "engine() failed: setOption('%s', %s) returned non-zero code\n", engine->optionCheck[i].name, engine->optionCheck[i].value ? "true" : "false");
	}
	for (int i = 0; i < engine->numberOfComboOptions; i++) {
		if (engine->optionCombo[i].value != engine->optionCombo[i].defaultValue) {
			for (int j = 0; j < MAX_UCI_OPTION_COMBO_VARS; j++) {
				if (strcmp(engine->optionCombo[i].value, engine->optionCombo[i].values[j]) == 0) {
					if (setOption(engine, engine->optionCombo[i].name, Combo, &(engine->optionCombo[i].value)))
						fprintf(stderr, "engine() failed: setOption('%s', %s) returned non-zero code\n", engine->optionCombo[i].name, engine->optionCombo[i].value);
					break;
				}
				if (j == MAX_UCI_OPTION_COMBO_VARS) {
					fprintf(stderr, "Combo option %s has no such value %s. Allowed values are: ", engine->optionCombo[i].name, engine->optionCombo[i].value);
					for (j = 0; j < MAX_UCI_OPTION_COMBO_VARS; j++)
						fprintf(stderr, " '%s'", engine->optionCombo[i].values[j]);
					fprintf(stderr, "\n");
				}
			}
		}
	}
	for (int i = 0; i < engine->numberOfSpinOptions; i++) {
		if (engine->optionSpin[i].value != engine->optionSpin[i].defaultValue)
			if (setOption(engine, engine->optionSpin[i].name, Spin, &(engine->optionSpin[i].value)))
				fprintf(stderr, "engine() failed: setOption(%s, %ld) returned non-zero code\n", engine->optionSpin[i].name, engine->optionSpin[i].value);
	}
	for (int i = 0; i < engine->numberOfStringOptions; i++) {
		if (strcmp(engine->optionString[i].value, engine->optionString[i].defaultValue) != 0)
			if (setOption(engine, engine->optionString[i].name, String, &(engine->optionString[i].value)))
				fprintf(stderr, "engine() failed: setOption('%s', '%s') returned non-zero code\n", engine->optionString[i].name, engine->optionString[i].value);
	}
}

bool isReady(struct Engine * engine) {
	bool ready = false;
	char line[256];
	fprintf(engine->toEngine, "isready\n");
	fflush(engine->toEngine);
	//fprintf(stderr, "isready\n");
	while (fgets(line, sizeof(line), engine->fromEngine)) {
		//fprintf(stderr, "%s", line);
		if (strcmp(line, "readyok\n") == 0) {
			ready = true;
			break;
		}
	}
	return ready;
}

bool newGame(struct Engine * engine) {
	fprintf(engine->toEngine, "ucinewgame\n");
	fflush(engine->toEngine);
	//fprintf(stderr, "ucinewgame\n");
	return isReady(engine);
}

void stop(struct Engine * engine) {
	fprintf(engine->toEngine, "stop\n");
	fflush(engine->toEngine);
	//fprintf(stderr, "stop\n");
}

void quit(struct Engine * engine) {
	fprintf(engine->toEngine, "quit\n");
	fflush(engine->toEngine);
	//fprintf(stderr, "quit\n");
	remove(engine->namedPipeTo);
	remove(engine->namedPipeFrom);
}

bool position(struct Engine * engine) {
	char line[5206];
	if (strlen(engine->position) >= 25) //min FEN length I think
		sprintf(line, "position fen %s", engine->position);
	else sprintf(line, "position startpos");
	if (strlen(engine->moves) >= 4) { //min uci move length
		strcat(line, " moves ");
		strncat(line, engine->moves, sizeof line - strlen(line));
	}
	fprintf(engine->toEngine, "%s\n", line);
	fflush(engine->toEngine);
	//fprintf(stderr, "%s\n", line);
	return isReady(engine);
}

int getPV(struct Engine * engine, struct Evaluation ** eval, int multiPV) {
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
	for (int i = 0; i < multiPV; i++) {
		unsigned char maxPlies = eval[i]->maxPlies;
	  memset(eval[i], 0, sizeof(struct Evaluation));
	  eval[i]->maxPlies = maxPlies;
  } 
	while (fgets(line, sizeof(line), engine->fromEngine)) {
		//fprintf(stderr, "%s", line);
		if (strstr(line, "bestmove ") - line == 0) {
			if (((sscanf(line, "bestmove %5s ponder %5s\n", eval[0]->bestmove, eval[0]->ponder) == 2) || (sscanf(line, "bestmove %5s\n", eval[0]->bestmove) == 1)) && strncmp(eval[0]->bestmove, "(none", 5) != 0) {
				for (int i = 0; i < multiPV; i++) {
					//printf("scanning prevLine[%d] %s with score cp\n", i, prevLine[i]);
					if (sscanf(prevLine[i], "info depth %hhu seldepth %hhu multipv %hhu score cp %d nodes %lu nps %lu hashfull %hu tbhits %hhu time %lu pv %1024[abcdefghnqr12345678\040]\n", &(eval[i]->depth), &(eval[i]->seldepth), &(eval[i]->multipv), &(eval[i]->scorecp), &(eval[i]->nodes), &(eval[i]->nps), &(eval[i]->hashful), &(eval[i]->tbhits), &(eval[i]->time), eval[i]->pv) != 10) {
  					//printf("scanning prevLine[%d] %s with score mate\n", i, prevLine[i]);
					  if (sscanf(prevLine[i], "info depth %hhu seldepth %hhu multipv %hhu score mate %d nodes %lu nps %lu hashfull %hu tbhits %hhu time %lu pv %1024[abcdefghnqr12345678\040]\n", &(eval[i]->depth), &(eval[i]->seldepth), &(eval[i]->multipv), &(eval[i]->matein), &(eval[i]->nodes), &(eval[i]->nps), &(eval[i]->hashful), &(eval[i]->tbhits), &(eval[i]->time), eval[i]->pv) == 10)
					    eval[i]->scorecp = MATE_SCORE * (eval[i]->matein/abs(eval[i]->matein));
					}
					//truncate pv string to maxPlies
					char * tmpLine = eval[i]->pv;
					int count = 0;
					while((tmpLine = strchr(tmpLine, ' ')) != NULL) {
            if (++count == eval[i]->maxPlies) {
            	tmpLine[0] = '\0';
            	break;
            }
            ++tmpLine; // Increment result, otherwise we'll find target at the same location
          }
					//tmpLine = strstr(prevLine[i], " mate ");
					//if (tmpLine) sscanf(tmpLine + 6, "%d", &(eval[0]->matein));
					//printf("freeing prevLine[%d]\n", i);
					free(prevLine[i]);
				}
				//scorecp is given from the point of view of sideToMove, meaning
				// negative scorecp is losing, positive - winning
				if (strlen(engine->position) >= 25) {
  				enum Color sideToMove;
					const char * position = engine->position;
					position = strchr(engine->position, ' ');
					sideToMove = position[1] == 'w' ? ColorWhite : ColorBlack;
					//White or Black has a slight advantage
					if (eval[0]->scorecp > INACCURACY && eval[0]->scorecp <= MISTAKE) {
						if (sideToMove == ColorWhite) eval[0]->nag = 14;
						else eval[0]->nag = 15;
					}
					//White or Black has a slight advantage
					else if (eval[0]->scorecp < -INACCURACY && eval[0]->scorecp >= -MISTAKE) {
						if (sideToMove == ColorWhite) eval[0]->nag = 15;
						else eval[0]->nag = 14;
					}
					//White or Black has a moderate advantage
					else if (eval[0]->scorecp > MISTAKE && eval[0]->scorecp <= BLUNDER) {
						if (sideToMove == ColorWhite) eval[0]->nag = 16;
						else eval[0]->nag = 17;
					}
					//White or Black has a moderate advantage
					else if (eval[0]->scorecp < -MISTAKE && eval[0]->scorecp >= -BLUNDER) {
						if (sideToMove == ColorWhite) eval[0]->nag = 17;
						else eval[0]->nag = 16;
					}
					//White or Black has a decisive advantage
					else if (eval[0]->scorecp > BLUNDER && eval[0]->scorecp < MATE_SCORE / 2) {
						if (sideToMove == ColorWhite) eval[0]->nag = 18;
						else eval[0]->nag = 19;
					}
					//White or Black has a decisive advantage
					else if (eval[0]->scorecp < -BLUNDER && eval[0]->scorecp > -MATE_SCORE / 2) {
						if (sideToMove == ColorWhite) eval[0]->nag = 19;
						else eval[0]->nag = 18;
					}
					//White or Black has a crushing advantage (one should resign)
					else if (eval[0]->scorecp >= MATE_SCORE / 2 && eval[0]->scorecp <= MATE_SCORE) {
					  if (sideToMove == ColorWhite) eval[0]->nag = 20;
					  else eval[0]->nag = 21;
					}
					//White or Black has a crushing advantage (one should resign)
					else if (eval[0]->scorecp <= -MATE_SCORE / 2 && eval[0]->scorecp >= -MATE_SCORE) {
						if (sideToMove == ColorWhite) eval[0]->nag = 21;
						else eval[0]->nag = 20;
					}
					//Drawish position
					else eval[0]->nag = 10;
				}
				return 0;
			} //end of if bestmove ponder || bestmove
		} //end of if bestmove
		else {
			if ((tmpLine = strstr(line, " multipv "))) {
				if (sscanf(tmpLine + 9, "%d ", &pv)) {
					if ((tmpLine = strstr(tmpLine, " pv "))) {
						tmpLine += 4;
						len = strlen(tmpLine) / 5; // this will give ply number (more or less)
						pv--; // PV numbers are 1-based, arrays are 0-based
						// save the longest PV, which is not greater than maxPlies
						// if it is greater, then truncate
						if (len > eval[pv]->maxPlies)
							len = eval[pv]->maxPlies;
					  if (len >= prevLen[pv]) {
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
	return 0;
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
//void go(long movetime, int depth, int nodes, int mate, char * searchmoves, bool ponder, bool infinite, long wtime, long btime, long winc, long binc, int movestogo, struct Engine * engine, struct Evaluation ** eval) {
void go(struct Engine * engine, struct Evaluation ** eval) {
	char line[4096], tmp[256];
	sprintf(line, "go");
	if (engine->movetime) {
		sprintf(tmp, " movetime %ld", engine->movetime);
		strcat(line, tmp);
	}
	if (engine->depth) {
		sprintf(tmp, " depth %d", engine->depth);
		strcat(line, tmp);
	}
	if (engine->nodes) {
		sprintf(tmp, " nodes %d", engine->nodes);
		strcat(line, tmp);
	}
	if (engine->mate) {
		sprintf(tmp, " mate %d", engine->mate);
		strcat(line, tmp);
	}
	if (engine->searchmoves) {
		sprintf(tmp, " searchmoves %s", engine->searchmoves);
		strcat(line, tmp);
	}
	if (engine->wtime) {
		sprintf(tmp, " wtime %ld", engine->wtime);
		strcat(line, tmp);
	}
	if (engine->btime) {
		sprintf(tmp, " btime %ld", engine->btime);
		strcat(line, tmp);
	}
	if (engine->winc) {
		sprintf(tmp, " winc %ld", engine->winc);
		strcat(line, tmp);
	}
	if (engine->binc) {
		sprintf(tmp, " binc %ld", engine->binc);
		strcat(line, tmp);
	}
	if (engine->movestogo) {
		sprintf(tmp, " movestogo %d", engine->movestogo);
		strcat(line, tmp);
	}
	if (engine->ponder) strcat(line, " engine->ponder");
	if (engine->infinite) strcat(line, " engine->infinite");

	fprintf(engine->toEngine, "%s\n", line);
	fflush(engine->toEngine);
	//fprintf(stderr, "%s\n", line);

	int multiPV = nametoindex(engine, "MultiPV", Spin);
	if (multiPV < 0) {
		fprintf(stderr, "engine() failed: nametoindex(MultiPV, Spin) return -1\n");
		return;
	}
	//fprintf(stderr, "multiPV %ld\n", engine->optionSpin[multiPV].value);
	getPV(engine, eval, engine->optionSpin[multiPV].value);
}

int engine(struct Engine * engine, char * engineName) {
  if (!engine) {
		printf("engine() error: engine argument is NULL\n");
  	return 1;
  }
	if (!(engineName)) {
		printf("engine() error: engineName is NULL\n");
		return 1;			
	}
	strcpy(engine->engineName, engineName);
	char symbols[62] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	char suffix[10];
	strcpy(engine->namedPipeTo, TO_ENGINE_NAMED_PIPE_PREFIX);
	strcpy(engine->namedPipeFrom, FROM_ENGINE_NAMED_PIPE_PREFIX);
	do {
		suffix[0] = '.';
		for (int i = 1; i < 9; i++) {
			suffix[i] = symbols[randomNumber(0, 61)];
		}
		suffix[9] = '\0';
		strncat(engine->namedPipeTo, suffix, 10);
		strncat(engine->namedPipeFrom, suffix, 10);
	} while(mkfifo(engine->namedPipeTo, S_IRUSR | S_IWUSR) == -1 || mkfifo(engine->namedPipeFrom, S_IRUSR | S_IWUSR) == -1);
	pid_t enginePid;
	//named pipes, i.e. fifo
/*	
	if (mkfifo(engine->namedPipeTo, S_IRUSR | S_IWUSR) == -1) {
		printf("engine() error: mkfifo(%s, S_IRUSR | S_IWUSR) returned %s\n", engine->namedPipeTo, strerror(errno));
		return 1;
	}
	if (mkfifo(engine->namedPipeFrom, S_IRUSR | S_IWUSR) == -1) {
		printf("engine() error: mkfifo(%s, S_IRUSR | S_IWUSR) returned %s\n", engine->namedPipeFrom, strerror(errno));
		return 1;
	}
*/	
	enginePid = fork();
	if (enginePid < 0) {
		printf("engine() error: fork failed\n");
		return 1;
	}
	if (enginePid == 0) { //child
		int toEngine, fromEngine;
		if ((toEngine = open(engine->namedPipeTo, O_RDONLY)) == -1) {
			printf("engine() child error: open(%s, O_RDONLY) returned %s\n", engine->namedPipeTo, strerror(errno));
			return 1;
		}
		//printf("engine() child: open(%s, O_RDONLY) ok\n", engine->namedPipeTo);
		if ((fromEngine = open(engine->namedPipeFrom, O_WRONLY)) == -1) {
			printf("engine() child error: open(%s, O_WRONLY) returned %s\n", engine->namedPipeFrom, strerror(errno));
			close(toEngine);
			remove(engine->namedPipeTo);
			return 1;
		}
		//printf("engine() child: open(%s, O_WRONLY) ok\n", engine->namedPipeFrom);
		dup2(toEngine, STDIN_FILENO);
		close(toEngine);
		dup2(fromEngine, STDOUT_FILENO);
		close(fromEngine);
		if (execlp((char *)engine->engineName, (char *)engine->engineName, (char *)NULL) < 0) {
			printf("engine() child error: execlp failed with %d (%s)\n", errno, strerror(errno));
			return 1;
		}
	}
	else { //parent
		if ((engine->toEngine = fopen(engine->namedPipeTo, "w")) == NULL) {
			printf("engine() parent error: fopen(%s, w) returned %s\n", engine->namedPipeTo, strerror(errno));
			return 1;
		}
		//printf("engine() parent: fopen(%s, w) ok\n", engine->namedPipeTo);
		if ((engine->fromEngine = fopen(engine->namedPipeFrom, "r")) == NULL) {
			printf("engine() parent error: fopen(%s, r) returned %s\n", engine->namedPipeFrom, strerror(errno));
			fclose(engine->toEngine);
			remove(engine->namedPipeTo);
			return 1;
		}
	}
	return 0;
}

struct Engine * initChessEngine(char * engineName, long movetime, int depth, int hashSize, int threadNumber, char * syzygyPath, int multiPV) {
  struct timespec delay;
  delay.tv_sec = 1;
  delay.tv_nsec = 0;
  struct Engine * chessEngine;
  chessEngine = malloc(sizeof(struct Engine));
  if (!chessEngine) {
    printf("initChessEngine() error: malloc(sizeof(struct Engine)) returned NULL\n");
    return NULL;  	
  }
  chessEngine->position[0] = '\0';
  chessEngine->moves[0] = '\0';
  chessEngine->movetime = movetime;
  chessEngine->depth = depth;
  chessEngine->nodes = 0;
  chessEngine->searchmoves = NULL;
  chessEngine->infinite = false;
  chessEngine->ponder = false;
  chessEngine->mate = 0;
  chessEngine->movestogo = 0;
  chessEngine->wtime = 0;
  chessEngine->btime = 0;
  chessEngine->winc = 0;
  chessEngine->binc = 0;
  int res = engine(chessEngine, engineName);
  if (res) {
    printf("initChessEngine() error: engine(%s) returned %d\n", engineName, res);
    return NULL;
  } 
  getOptions(chessEngine);
  int idx = nametoindex(chessEngine, "MultiPV", Spin);
  chessEngine->optionSpin[idx].value = multiPV;
  idx = nametoindex(chessEngine, "Hash", Spin);
  chessEngine->optionSpin[idx].value = hashSize;
  idx = nametoindex(chessEngine, "Threads", Spin);
  chessEngine->optionSpin[idx].value = threadNumber;
  idx = nametoindex(chessEngine, "SyzygyPath", String);
  strncpy(chessEngine->optionString[idx].value, syzygyPath, MAX_UCI_OPTION_STRING_LEN);
  setOptions(chessEngine);
  int timeout = 0;
  while (!isReady(chessEngine) && timeout < 3) {
    printf("initChessEngine() warning: isReady() returned false\n");
    nanosleep(&delay, NULL);
    timeout++;
  }
  if (!isReady(chessEngine)) {
  	free(chessEngine);
    printf("initChessEngine() error: isReady() returned false\n");
  	return NULL;
  }
  return chessEngine;
}
