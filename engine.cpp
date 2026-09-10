#include <fcntl.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <thread>

//Every read from the engine used to be an unbounded fgets(), so a wedged or half-dead
//engine hung the calling thread forever. For the bot that means a rated game silently
//running out of clock with no error raised anywhere. poll() the descriptor first and
//give up after ENGINE_READ_TIMEOUT_MS; callers already treat a failed read as engine
//death and restart it. fromEngine is opened unbuffered so poll() cannot miss a line
//that stdio has already pulled into its own buffer.
#define ENGINE_READ_TIMEOUT_MS 60000
//Discard anything the engine has already written but nobody has read.
//
//A UCI engine emits exactly one "bestmove" per "go", and anything still unread when the next go
//is sent can only belong to a previous command -- most plausibly a bestmove produced in response
//to a "stop" that the caller did not consume. Reading it as this search's answer would return a
//move computed for the previous position, and leave the reads one further out of step each time.
//
//This was originally justified by an apparent 4.3% rate of duplicated bestmove lines in
//creatica.log. That was an ARTEFACT: two bots shared one log file, and creatica ponders the
//position where the opponent is to move -- the same position the opponent is searching for real
//-- so two engines' identical conclusions interleaved into what looked like one engine answering
//twice. The engines now log separately. The guard is kept because it is correct regardless, but
//it is not evidence of a bug in the engine.
static int drainEngine(FILE * f) {
	if (!f) return 0;
	const int fd = fileno(f);
	int dropped = 0;
	char buf[4096];
	for (;;) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		if (poll(&pfd, 1, 0) <= 0) break;          //nothing pending
		if (!(pfd.revents & POLLIN)) break;
		if (!fgets(buf, sizeof buf, f)) break;
		++dropped;
		if (dropped > 4096) break;                 //never spin on a flood
	}
	return dropped;
}

static char * engineFgets(char * buf, int size, FILE * f) {
	if (!f) return nullptr;
	for (;;) {
		struct pollfd pfd;
		pfd.fd = fileno(f);
		pfd.events = POLLIN;
		pfd.revents = 0;
		int r = poll(&pfd, 1, ENGINE_READ_TIMEOUT_MS);
		if (r > 0) return fgets(buf, size, f);
		if (r == 0) {
			fprintf(stderr, "engineFgets() error: engine produced nothing for %d ms - treating it as dead\n", ENGINE_READ_TIMEOUT_MS);
			return nullptr;
		}
		if (errno == EINTR) continue; //a signal, not a failure
		fprintf(stderr, "engineFgets() error: poll(): %s\n", strerror(errno));
		return nullptr;
	}
}
#include "nnue/bitboard.h"
#include "libchess.h"

//#ifdef __cplusplus
//extern "C" {
//#endif

#ifndef __APPLE__ // macOS
unsigned int arc4random_uniform(unsigned int upper_bound) {
    if (upper_bound == 0) return 0;
    unsigned int min = -upper_bound % upper_bound; // Compute rejection threshold
    unsigned int r;
    do {
        r = rand();
    } while (r < min); // Reject to ensure uniformity
    return r % upper_bound;
}
#endif

int randomNumber(const int min, const int max) {
    const int range = max - min + 1;
    const unsigned int max_random = 0xFFFFFFFFU; // Maximum value of random()
    const unsigned int threshold = (max_random / range) * range;
    unsigned int num;
    do {
        num = arc4random_uniform(max + 1);
    } while (num >= threshold); // Reject numbers causing bias
    return (num % range) + min; // Adjust to desired range
}

int getOptions(Engine& engine) {
	char name[MAX_UCI_OPTION_NAME_LEN], type[MAX_UCI_OPTION_TYPE_LEN],
		defaultStringValue[MAX_UCI_OPTION_STRING_LEN];
	long long defaultSpinValue, minValue, maxValue;

	fprintf(engine.toEngine, "uci\n");
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "uci\n");
		fflush(engine.logfile);
	}
	char line[256];
	engine.numberOfCheckOptions = 0;
	engine.numberOfComboOptions = 0;
	engine.numberOfSpinOptions = 0;
	engine.numberOfStringOptions = 0;
	engine.numberOfButtonOptions = 0;
	char * lineMod = NULL, * tmp = NULL;
	while (engineFgets(line, sizeof(line), engine.fromEngine)) {
		if (engine.logfile) {
			fprintf(engine.logfile, "%s", line);
			fflush(engine.logfile);
		}
		// Trim trailing \r and/or \n
		char* end = line + strlen(line) - 1;
		while (end >= line && (*end == '\r' || *end == '\n')) {
			*end = '\0';
			end--;
		}
		if (strcmp(line, "uciok") == 0) break;
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
								if (engine.numberOfButtonOptions < MAX_UCI_OPTION_BUTTON_NUM)
									strcpy(engine.optionButton[engine.numberOfButtonOptions++].name, name);
								else {
									fprintf(stderr, "getOptions() warning: number of button options exceeded the maximum of %d; ignoring it and continuing\n", MAX_UCI_OPTION_BUTTON_NUM);
									//NOT a return. Aborting the parse here discarded every option after this one, of
									//every type: creatica's seventeenth SPIN option is advertised before its
									//VisitDumpFile STRING option, so overflowing the spin array silently lost an
									//unrelated string option and the driver reported only that the engine did not
									//advertise it. Losing the one option that does not fit is bad; losing the rest of
									//the list because of it is worse, and far harder to diagnose.
								}
								break;
							case 1: //check
								res = sscanf(lineMod, "check default %5s\n", defaultStringValue);
								if (res < 1) {
									fprintf(stderr, "getOptions() error: unable to parse the check option line '%s'", line);
									return 1;
								}
								if (engine.numberOfCheckOptions < MAX_UCI_OPTION_CHECK_NUM) {
									strcpy(engine.optionCheck[engine.numberOfCheckOptions].name, name);
									if (strcmp(defaultStringValue, "false") == 0) {
										engine.optionCheck[engine.numberOfCheckOptions].defaultValue = false;
										engine.optionCheck[engine.numberOfCheckOptions++].value = false;
									}
									else if (strcmp(defaultStringValue, "true") == 0) {
										engine.optionCheck[engine.numberOfCheckOptions].defaultValue = true;
										engine.optionCheck[engine.numberOfCheckOptions++].value = true;
									}
								} else {
									fprintf(stderr, "getOptions() warning: number of check options exceeded the maximum of %d; ignoring it and continuing\n", MAX_UCI_OPTION_CHECK_NUM);
									//NOT a return. Aborting the parse here discarded every option after this one, of
									//every type: creatica's seventeenth SPIN option is advertised before its
									//VisitDumpFile STRING option, so overflowing the spin array silently lost an
									//unrelated string option and the driver reported only that the engine did not
									//advertise it. Losing the one option that does not fit is bad; losing the rest of
									//the list because of it is worse, and far harder to diagnose.
								}
								break;
							case 2: //combo
								res = sscanf(lineMod, "combo default %31s", defaultStringValue);
								if (res < 1) {
									fprintf(stderr, "getOptions() error: unable to parse the combo option line '%s'", line);
									return 1;
								}
								if (engine.numberOfComboOptions < MAX_UCI_OPTION_COMBO_NUM) {
									strcpy(engine.optionCombo[engine.numberOfComboOptions].name, name);
									strcpy(engine.optionCombo[engine.numberOfComboOptions].defaultValue, defaultStringValue);
									memset(engine.optionCombo[engine.numberOfComboOptions].values, 0, sizeof(char[MAX_UCI_OPTION_COMBO_VARS][MAX_UCI_OPTION_STRING_LEN]));
									char * var;
									int n = 0;
									var = line;
									while ((var = strstr(var, " var "))) {
										res = sscanf(var, " var %31s", defaultStringValue);
										if (res == 1 && n < MAX_UCI_OPTION_COMBO_VARS)
											strcpy(engine.optionCombo[engine.numberOfComboOptions].values[n++], defaultStringValue);
										var += 5;
									}
									engine.numberOfComboOptions++;
								} else {
									fprintf(stderr, "getOptions() warning: number of combo options exceeded the maximum of %d; ignoring it and continuing\n", MAX_UCI_OPTION_COMBO_NUM);
									//NOT a return. Aborting the parse here discarded every option after this one, of
									//every type: creatica's seventeenth SPIN option is advertised before its
									//VisitDumpFile STRING option, so overflowing the spin array silently lost an
									//unrelated string option and the driver reported only that the engine did not
									//advertise it. Losing the one option that does not fit is bad; losing the rest of
									//the list because of it is worse, and far harder to diagnose.
								}
								break;
							case 3: //spin
								res = sscanf(lineMod, "spin default %lld min %lld max %lld\n", &defaultSpinValue, &minValue, &maxValue);
								if (res < 3) {
									fprintf(stderr, "getOptions() error: unable to parse the spin option line '%s'", line);
									return 1;
								}
								if (engine.numberOfSpinOptions < MAX_UCI_OPTION_SPIN_NUM) {
									strcpy(engine.optionSpin[engine.numberOfSpinOptions].name, name);
									engine.optionSpin[engine.numberOfSpinOptions].defaultValue = defaultSpinValue;
									engine.optionSpin[engine.numberOfSpinOptions].value = defaultSpinValue;
									engine.optionSpin[engine.numberOfSpinOptions].min = minValue;
									engine.optionSpin[engine.numberOfSpinOptions++].max = maxValue;
								} else {
									fprintf(stderr, "getOptions() warning: number of spin options exceeded the maximum of %d; ignoring it and continuing\n", MAX_UCI_OPTION_SPIN_NUM);
									//NOT a return. Aborting the parse here discarded every option after this one, of
									//every type: creatica's seventeenth SPIN option is advertised before its
									//VisitDumpFile STRING option, so overflowing the spin array silently lost an
									//unrelated string option and the driver reported only that the engine did not
									//advertise it. Losing the one option that does not fit is bad; losing the rest of
									//the list because of it is worse, and far harder to diagnose.
								}
								break;
							case 4: //string
								res = sscanf(lineMod, "string default %31s\n", defaultStringValue);
								if (res < 1) {
									//fprintf(stderr, "getOptions() warning: unable to parse the string option line '%s'", line);
									defaultStringValue[0] = '\0';
								}
								if (engine.numberOfStringOptions < MAX_UCI_OPTION_STRING_NUM) {
									strcpy(engine.optionString[engine.numberOfStringOptions].name, name);
									strcpy(engine.optionString[engine.numberOfStringOptions].defaultValue, defaultStringValue);
									strcpy(engine.optionString[engine.numberOfStringOptions++].value, defaultStringValue);
								} else {
									fprintf(stderr, "getOptions() warning: number of string options exceeded the maximum of %d; ignoring it and continuing\n", MAX_UCI_OPTION_STRING_NUM);
									//NOT a return. Aborting the parse here discarded every option after this one, of
									//every type: creatica's seventeenth SPIN option is advertised before its
									//VisitDumpFile STRING option, so overflowing the spin array silently lost an
									//unrelated string option and the driver reported only that the engine did not
									//advertise it. Losing the one option that does not fit is bad; losing the rest of
									//the list because of it is worse, and far harder to diagnose.
								}
								break;
							}
							break;
						}
					}
				}
			}
		}
		//Both fields used to be copied with strncpy(dst, src, sizeof dst) and then have their
		//LAST character deleted unconditionally to strip the newline. Two defects: a source at
		//least as long as the buffer left it with no terminator, so the strlen() on the next
		//line read past the end; and when the line arrived already stripped, the delete ate a
		//real character -- an engine identifying itself as "StubEngine" was recorded as
		//"StubEngin". Copy one byte short, terminate, then remove a newline only if there is one.
		else if (strstr(line, "id name ") - line == 0) {
			strncpy(engine.id, line + 8, MAX_UCI_OPTION_STRING_LEN - 1);
			engine.id[MAX_UCI_OPTION_STRING_LEN - 1] = '\0';
			for (size_t n = strlen(engine.id); n && (engine.id[n - 1] == '\n' || engine.id[n - 1] == '\r'); --n)
				engine.id[n - 1] = '\0';
		}
		else if (strstr(line, "id author ") - line == 0) {
			strncpy(engine.authors, line + 10, 2 * MAX_UCI_OPTION_STRING_LEN - 1);
			engine.authors[2 * MAX_UCI_OPTION_STRING_LEN - 1] = '\0';
			for (size_t n = strlen(engine.authors); n && (engine.authors[n - 1] == '\n' || engine.authors[n - 1] == '\r'); --n)
				engine.authors[n - 1] = '\0';
		}
	}
	return 0;
}

int nametoindex(const Engine& engine, const char * name, OptionType type) {
	int idx = -1;
	switch (type) {
	case Button: //button
		for (int i = 0; i < engine.numberOfButtonOptions; i++) {
			if (strncmp(engine.optionButton[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case Check: //check
		for (int i = 0; i < engine.numberOfCheckOptions; i++) {
			if (strncmp(engine.optionCheck[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case Combo: //combo
		for (int i = 0; i < engine.numberOfComboOptions; i++) {
			if (strncmp(engine.optionCombo[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case Spin: //spin
		for (int i = 0; i < engine.numberOfSpinOptions; i++) {
			if (strncmp(engine.optionSpin[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	case String: //string
		for (int i = 0; i < engine.numberOfStringOptions; i++) {
			if (strncmp(engine.optionString[i].name, name, MAX_UCI_OPTION_NAME_LEN) == 0) {
				idx = i;
				break;
			}
		}
		break;
	}
	/*
	if (idx < 0) {
		fprintf(stderr, "nametoindex() warning: engine %s does not know option %s of type %s\n", engine.id, name, optionTypes[type]);
	}*/
	return idx;
}

int setOption(const Engine& engine, const char * name, OptionType type, void * value) {
	char line[256];
	line[0] = '\0';
	long long v;
	int idx;
	bool val;

	switch (type) {
	case Button: //button
		idx = nametoindex(engine, name, Button);
		if (idx < 0) return 1;
		sprintf(line, "setoption name %s", name);
		break;
	case Check: //check
		idx = nametoindex(engine, name, Check);
		if (idx < 0) return 1;
		val = *(bool *)value;
		sprintf(line, "setoption name %s value %s", name, val ? "true" : "false");
		break;
	case Combo: //combo
		idx = nametoindex(engine, name, Combo);
		if (idx < 0) return 1;
		sprintf(line, "setoption name %s value %s", name, (char *)value);
		break;
	case Spin: //spin
		v = *(long long *)value;
		idx = nametoindex(engine, name, Spin);
		if (idx < 0) return 1;
		if (v <= engine.optionSpin[idx].max && v >= engine.optionSpin[idx].min)
			sprintf(line, "setoption name %s value %lld", name, v);
		else {
			fprintf(stderr, "setOption() failed: spin option %s value (%lld) is outside min-max range: %lld - %lld\n", name, v, engine.optionSpin[idx].min, engine.optionSpin[idx].max);
			return 1;
		}
		break;
	case String: //string
		idx = nametoindex(engine, name, String);
		if (idx < 0) return 1;
		sprintf(line, "setoption name %s value %s", name, (char *)value);
		break;
	}
	fprintf(engine.toEngine, "%s\n", line);
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "%s\n", line);
		fflush(engine.logfile);
	}
	// without any output from the engine, fgets() will block
	//while (fgets(line, sizeof(line), stdin)) {
	//	fprintf(stderr, "%s", line);
	//}
	return 0;
}

//Set an option by name. See the comment on the declarations in libchess.h for why indexing
//optionSpin[]/optionCheck[] with the EngineSpinOptions enum was wrong.
bool setEngineSpin(Engine& engine, const char * name, int64_t value) {
	const int idx = nametoindex(engine, name, Spin);
	if (idx < 0) {
		fprintf(stderr, "setEngineSpin() warning: engine %s does not advertise spin option '%s' - ignored\n",
		        engine.id[0] ? engine.id : engine.engineName, name);
		return false;
	}
	const int64_t lo = engine.optionSpin[idx].min, hi = engine.optionSpin[idx].max;
	if (value < lo || value > hi) {
		fprintf(stderr, "setEngineSpin() warning: %s = %lld is outside the advertised range %lld..%lld, clamped\n",
		        name, (long long)value, (long long)lo, (long long)hi);
		value = value < lo ? lo : hi;
	}
	engine.optionSpin[idx].value = value;
	return true;
}

bool setEngineCheck(Engine& engine, const char * name, bool value) {
	const int idx = nametoindex(engine, name, Check);
	if (idx < 0) {
		fprintf(stderr, "setEngineCheck() warning: engine %s does not advertise check option '%s' - ignored\n",
		        engine.id[0] ? engine.id : engine.engineName, name);
		return false;
	}
	engine.optionCheck[idx].value = value;
	return true;
}

bool setEngineStringOption(Engine& engine, const char * name, const char * value) {
	const int idx = nametoindex(engine, name, String);
	if (idx < 0) {
		fprintf(stderr, "setEngineStringOption() warning: engine %s does not advertise string option '%s' - ignored\n",
		        engine.id[0] ? engine.id : engine.engineName, name);
		return false;
	}
	strncpy(engine.optionString[idx].value, value ? value : "", MAX_UCI_OPTION_STRING_LEN - 1);
	engine.optionString[idx].value[MAX_UCI_OPTION_STRING_LEN - 1] = '\0';
	return true;
}

bool engineSpinRange(const Engine& engine, const char * name,
                     int64_t& lo, int64_t& hi, int64_t& def) {
	const int idx = nametoindex(engine, name, Spin);
	if (idx < 0) return false;
	lo  = engine.optionSpin[idx].min;
	hi  = engine.optionSpin[idx].max;
	def = engine.optionSpin[idx].defaultValue;
	return true;
}

void setOptions(const Engine& engine) {
	for (int i = 0; i < engine.numberOfButtonOptions; i++) {
		if (engine.optionButton[i].value)
			if (setOption(engine, engine.optionButton[i].name, Button, NULL))
				fprintf(stderr, "engine() warning: setOption('%s') returned non-zero code\n", engine.optionButton[i].name);
	}
	for (int i = 0; i < engine.numberOfCheckOptions; i++) {
		if (engine.optionCheck[i].value != engine.optionCheck[i].defaultValue)
			if (setOption(engine, engine.optionCheck[i].name, Check, (void *)&(engine.optionCheck[i].value)))
				fprintf(stderr, "engine() failed: setOption('%s', %s) returned non-zero code\n", engine.optionCheck[i].name, engine.optionCheck[i].value ? "true" : "false");
	}
	for (int i = 0; i < engine.numberOfComboOptions; i++) {
		if (strncmp(engine.optionCombo[i].value, engine.optionCombo[i].defaultValue, MAX_UCI_OPTION_STRING_LEN) != 0) {
			for (int j = 0; j < MAX_UCI_OPTION_COMBO_VARS; j++) {
				if (strcmp(engine.optionCombo[i].value, engine.optionCombo[i].values[j]) == 0) {
					if (setOption(engine, engine.optionCombo[i].name, Combo, (void *)&(engine.optionCombo[i].value)))
						fprintf(stderr, "engine() failed: setOption('%s', %s) returned non-zero code\n", engine.optionCombo[i].name, engine.optionCombo[i].value);
					break;
				}
				if (j == MAX_UCI_OPTION_COMBO_VARS) {
					fprintf(stderr, "Combo option %s has no such value %s. Allowed values are: ", engine.optionCombo[i].name, engine.optionCombo[i].value);
					for (j = 0; j < MAX_UCI_OPTION_COMBO_VARS; j++)
						fprintf(stderr, " '%s'", engine.optionCombo[i].values[j]);
					fprintf(stderr, "\n");
				}
			}
		}
	}
	for (int i = 0; i < engine.numberOfSpinOptions; i++) {
		if (engine.optionSpin[i].value != engine.optionSpin[i].defaultValue)
			if (setOption(engine, engine.optionSpin[i].name, Spin, (void *)&(engine.optionSpin[i].value)))
				fprintf(stderr, "engine() failed: setOption(%s, %lld) returned non-zero code\n", engine.optionSpin[i].name, engine.optionSpin[i].value);
	}
	for (int i = 0; i < engine.numberOfStringOptions; i++) {
		if (strcmp(engine.optionString[i].value, engine.optionString[i].defaultValue) != 0)
			if (setOption(engine, engine.optionString[i].name, String, (void *)&(engine.optionString[i].value)))
				fprintf(stderr, "engine() failed: setOption('%s', '%s') returned non-zero code\n", engine.optionString[i].name, engine.optionString[i].value);
	}
}

bool isReady(const Engine& engine) {
	bool ready = false;
	char line[256];
	fprintf(engine.toEngine, "isready\n");
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "isready\n");
		fflush(engine.logfile);
	}
	while (engineFgets(line, sizeof(line), engine.fromEngine)) {
		if (engine.logfile) {
			fprintf(engine.logfile, "%s", line);
			fflush(engine.logfile);
		}
		// Trim trailing \r and/or \n
		char* end = line + strlen(line) - 1;
		while (end >= line && (*end == '\r' || *end == '\n')) {
			*end = '\0';
			end--;
		}
		if (strcmp(line, "readyok") == 0) {
			ready = true;
			break;
		}
	}
	return ready;
}

bool newGame(const Engine& engine) {
	fprintf(engine.toEngine, "ucinewgame\n");
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "ucinewgame\n");
		fflush(engine.logfile);
	}
	return isReady(engine);
}

int pieces(const Engine& engine) {
	fprintf(engine.toEngine, "pieces\n");
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "pieces\n");
		fflush(engine.logfile);
	}
	char line[256];	
	int pieceNumber = 0;
	while (engineFgets(line, sizeof(line), engine.fromEngine)) {
		if (engine.logfile) {
			fprintf(engine.logfile, "%s", line);
			fflush(engine.logfile);
		}
		// Trim trailing \r and/or \n
		char* end = line + strlen(line) - 1;
		while (end >= line && (*end == '\r' || *end == '\n')) {
			*end = '\0';
			end--;
		}
		pieceNumber = atoi(line);
		if (pieceNumber > 0) break;
	}
	return pieceNumber;	
}

void stop(const Engine& engine) {
	fprintf(engine.toEngine, "stop\n");
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "stop\n");
		fflush(engine.logfile);
	}
}

void quit(const Engine& engine) {
	fprintf(engine.toEngine, "quit\n");
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "quit\n");
		fflush(engine.logfile);
		fclose(engine.logfile);
	}
	fclose(engine.toEngine);
	fclose(engine.fromEngine);
	remove(engine.namedPipeTo);
	remove(engine.namedPipeFrom);
}

bool position(const Engine& engine) {
	char line[5206];
	if (strlen(engine.position) >= 25) //min FEN length I think
		sprintf(line, "position fen %s", engine.position);
	else sprintf(line, "position startpos");
	if (strlen(engine.moves) >= 4) { //min uci move length
		strcat(line, " moves ");
		strncat(line, engine.moves, sizeof line - strlen(line) - 1);
	}
	fprintf(engine.toEngine, "%s\n", line);
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "%s\n", line);
		fflush(engine.logfile);
	}
	return isReady(engine);
}

int getPV(const Engine& engine, struct Evaluation ** eval, const int multiPV) {
	char line[4096];
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
	enum Color sideToMove;
	sideToMove = strchr(engine.position, 'w') ? ColorWhite : ColorBlack;
	while (engineFgets(line, sizeof(line), engine.fromEngine)) {
		if (engine.logfile) {
			fprintf(engine.logfile, "%s", line);
			fflush(engine.logfile);
		}
		// Trim trailing \r and/or \n
		char* end = line + strlen(line) - 1;
		while (end >= line && (*end == '\r' || *end == '\n')) {
			*end = '\0';
			end--;
		}
		if (strstr(line, "bestmove ") - line == 0) {
			if ((sscanf(line, "bestmove %5s ponder %5s", eval[0]->bestmove, eval[0]->ponder) == 2) || (sscanf(line, "bestmove %5s", eval[0]->bestmove) == 1)) {
				if (strncmp(eval[0]->bestmove, "(none", 5) == 0 || eval[0]->bestmove[0] == '\0') {
					for (int i = 0; i < multiPV; i++) {
					  if (prevLine[i]) free(prevLine[i]);
					}
					if (engine.logfile) fprintf(engine.logfile, "getPV() error: bestmove is either (none) or blank:\n");
					if (engine.logfile) fprintf(engine.logfile, "%s", line);
					fprintf(stderr, "getPV() error: bestmove is either (none) or blank\n");
					return 1;
				}
				for (int i = 0; i < multiPV; i++) {
					if (!prevLine[i]) {
						for (int j = i - 1; j >= 0; j--) {
						  free(prevLine[j]);
						  prevLine[j] = NULL;	
						}		  
						break;
					}
					if (sscanf(prevLine[i], "info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %llu hashfull %d tbhits %d time %llu pv %1024[abcdefghnqr12345678\040]", &(eval[i]->depth), &(eval[i]->seldepth), &(eval[i]->multipv), &(eval[i]->scorecp), &(eval[i]->nodes), &(eval[i]->nps), &(eval[i]->hashful), &(eval[i]->tbhits), &(eval[i]->time), eval[i]->pv) != 10) {
					  if (sscanf(prevLine[i], "info depth %d seldepth %d multipv %d score mate %d nodes %llu nps %llu hashfull %d tbhits %d time %llu pv %1024[abcdefghnqr12345678\040]", &(eval[i]->depth), &(eval[i]->seldepth), &(eval[i]->multipv), &(eval[i]->matein), &(eval[i]->nodes), &(eval[i]->nps), &(eval[i]->hashful), &(eval[i]->tbhits), &(eval[i]->time), eval[i]->pv) == 10)
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
					free(prevLine[i]);
					prevLine[i] = NULL;
				} //end of for multiPV
				//scorecp is given from the point of view of sideToMove, meaning
				// negative scorecp is losing, positive - winning
				//White or Black has a slight advantage
				if (eval[0]->scorecp > INACCURACY && eval[0]->scorecp <= MISTAKE)
					eval[0]->nag = sideToMove == ColorWhite ? 14 : 15;
				//White or Black has a slight advantage
				else if (eval[0]->scorecp < -INACCURACY && eval[0]->scorecp >= -MISTAKE)
					eval[0]->nag = sideToMove == ColorWhite ? 15 : 14;
				//White or Black has a moderate advantage
				else if (eval[0]->scorecp > MISTAKE && eval[0]->scorecp <= BLUNDER)
					eval[0]->nag = sideToMove == ColorWhite ? 16 : 17;				
				//White or Black has a moderate advantage
				else if (eval[0]->scorecp < -MISTAKE && eval[0]->scorecp >= -BLUNDER)
					eval[0]->nag = sideToMove == ColorWhite ? 17 : 16;
				//White or Black has a decisive advantage
				else if (eval[0]->scorecp > BLUNDER && eval[0]->scorecp < MATE_SCORE / 2)
					eval[0]->nag = sideToMove == ColorWhite ? 18 : 19;
				//White or Black has a decisive advantage
				else if (eval[0]->scorecp < -BLUNDER && eval[0]->scorecp > -MATE_SCORE / 2)
					eval[0]->nag = sideToMove == ColorWhite ? 19 : 18;
				//White or Black has a crushing advantage (one should resign)
				else if (eval[0]->scorecp >= MATE_SCORE / 2 && eval[0]->scorecp <= MATE_SCORE)
				  eval[0]->nag = sideToMove == ColorWhite ? 20 : 21;
				//White or Black has a crushing advantage (one should resign)
				else if (eval[0]->scorecp <= -MATE_SCORE / 2 && eval[0]->scorecp >= -MATE_SCORE)
					eval[0]->nag = sideToMove == ColorWhite ? 21 : 20;
				//Drawish position
				else eval[0]->nag = 10;
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
							if (prevLine[pv]) {
								free(prevLine[pv]);
								prevLine[pv] = NULL;
							}
							prevLine[pv] = (char *)malloc(strlen(line) + 1);
							strcpy(prevLine[pv], line);
						}
					} //end of if pv
				} //end of if pv number
			} //end of if (muptipv)
			else if ((tmpLine = strstr(line, " score mate 0"))) { //if (neither bestmove, nor multipv)
			//this is usually when is mate or stalemate, the last line before bestmove (none) is
			// either "info depth 0 score mate 0" or "info depth 0 score cp 0"
					eval[0]->depth = 0;
					eval[0]->matein = 0;
					strcpy(eval[0]->bestmove, "none");
					if (sideToMove == ColorWhite) {
					  eval[0]->scorecp = -MATE_SCORE;
					  eval[0]->nag = 21; //there is no nag for mate
					} else {
					  eval[0]->scorecp = MATE_SCORE;						
  					eval[0]->nag = 20; //there is no nag for mate
					}
					return 0;
        } else if ((tmpLine = strstr(line, " score cp 0"))) {//info depth 0 score cp 0 (stalemate)
					eval[0]->depth = 0;
					strcpy(eval[0]->bestmove, "none");
					eval[0]->scorecp = 0;
					eval[0]->nag = 10;
					return 0;        	
        }				
		} //end of else (not bestmove)
	} //end of while (fgets(line...))
	if (feof(engine.fromEngine)) {
	  fprintf(stderr, "getPV(): fgets() reached EOF in engine.fromEngine pipe\n"); //engine crashed
	  return 1;
	}
	else if (ferror(engine.fromEngine)) {
	  fprintf(stderr, "getPV() error: fgets() failed to read from engine.fromEngine pipe\n");
	  return 1;
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
//void go(long long movetime, int depth, int nodes, int mate, char * searchmoves, bool ponder, bool infinite, long long wtime, long btime, long winc, long binc, int movestogo, struct Engine * engine, struct Evaluation ** eval) {
int go(const Engine& engine, struct Evaluation ** eval) {
	assert(eval);

	char line[4096], tmp[256];
	sprintf(line, "go");
	if (engine.movetime) {
		sprintf(tmp, " movetime %lld", engine.movetime);
		strcat(line, tmp);
	}
	if (engine.depth) {
		sprintf(tmp, " depth %d", engine.depth);
		strcat(line, tmp);
	}
	if (engine.nodes) {
		sprintf(tmp, " nodes %llu", engine.nodes);
		strcat(line, tmp);
	}
	if (engine.mate) {
		sprintf(tmp, " mate %d", engine.mate);
		strcat(line, tmp);
	}
	if (engine.searchmoves) {
		sprintf(tmp, " searchmoves %s", engine.searchmoves);
		strcat(line, tmp);
	}
	if (engine.wtime) {
		sprintf(tmp, " wtime %lld", engine.wtime);
		strcat(line, tmp);
	}
	if (engine.btime) {
		sprintf(tmp, " btime %lld", engine.btime);
		strcat(line, tmp);
	}
	if (engine.winc) {
		sprintf(tmp, " winc %lld", engine.winc);
		strcat(line, tmp);
	}
	if (engine.binc) {
		sprintf(tmp, " binc %lld", engine.binc);
		strcat(line, tmp);
	}
	if (engine.movestogo) {
		sprintf(tmp, " movestogo %d", engine.movestogo);
		strcat(line, tmp);
	}
	if (engine.ponder) strcat(line, " ponder");
	if (engine.infinite) strcat(line, " infinite");

	//Anything still unread belongs to a previous command, so it can only mislead this one.
	{
		const int stale = drainEngine(engine.fromEngine);
		if (stale && engine.logfile) {
			fprintf(engine.logfile, "go(): discarded %d stale line(s) before searching\n", stale);
			fflush(engine.logfile);
		}
	}
	fprintf(engine.toEngine, "%s\n", line);
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "%s\n", line);
		fflush(engine.logfile);
	}
	if (engine.infinite) return 0;
	int multiPV = nametoindex(engine, "MultiPV", Spin);
	if (multiPV < 0) {
		if (engine.logfile) {
			fprintf(engine.logfile, "engine() failed: nametoindex(MultiPV, Spin) return -1\n");
			fflush(engine.logfile);
		}
		fprintf(stderr, "engine() failed: nametoindex(MultiPV, Spin) return -1\n");
		return 1;
	}
	return getPV(engine, eval, engine.optionSpin[multiPV].value);
}

float getEval(const Engine& engine) {
	char line[2048];
	//char * tmpLine;
	float score = 0;
	while (engineFgets(line, sizeof(line), engine.fromEngine)) {
		if (engine.logfile) {
			fprintf(engine.logfile, "%s", line);
			fflush(engine.logfile);
		}
		if (strstr(line, "Final evaluation") - line == 0) {
			char * score_start = strpbrk(line, "+-");
			score = strtof(score_start, NULL);
			break;
		} 
	}
	return score;
}
//returns Stockfish eval in pawns from White's perspective
float eval(const Engine& engine) {
	fprintf(engine.toEngine, "eval\n");
	fflush(engine.toEngine);
	if (engine.logfile) {
		fprintf(engine.logfile, "eval\n");
		fflush(engine.logfile);
	}
	return getEval(engine);
}

int engine(Engine& engine, const char * engineName) {
    if (!engineName || strlen(engineName) >= MAX_ENGINE_NAME_LEN) {
        fprintf(stderr, "engine() error: invalid engineName\n");
        return 1;
    }
    strncpy(engine.engineName, engineName, MAX_ENGINE_NAME_LEN - 1);
    engine.engineName[MAX_ENGINE_NAME_LEN - 1] = '\0';

	// Generate unique pipe names
	const char symbols[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	char suffix[10];

#ifdef _WIN32
#include <windows.h>
#include <io.h>   // For _open_osfhandle, _close

	//To ensure random seeding, run srand(time(NULL)); from main()
	// Generate toEngine pipe name
    suffix[0] = '.';
    for (int i = 1; i < 9; i++) {
        suffix[i] = symbols[randomNumber(0, 61)];
    }
    suffix[9] = '\0';
    snprintf(engine.namedPipeTo, MAX_PIPE_NAME_LEN, "%s%s", TO_ENGINE_NAMED_PIPE_PREFIX, suffix);

    // Generate fromEngine pipe name
    suffix[0] = '.';
    for (int i = 1; i < 9; i++) {
        suffix[i] = symbols[randomNumber(0, 61)];
    }
    suffix[9] = '\0';
    snprintf(engine.namedPipeFrom, MAX_PIPE_NAME_LEN, "%s%s", FROM_ENGINE_NAMED_PIPE_PREFIX, suffix);

    // Create named pipes
    engine.hPipeToEngine = CreateNamedPipeA(
        engine.namedPipeTo,
        PIPE_ACCESS_OUTBOUND, // Parent writes to this pipe
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, // Max instances
        4096, // Out buffer size
        4096, // In buffer size
        0, // Default timeout
        NULL // Security attributes
    );
    if (engine.hPipeToEngine == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "engine() error: CreateNamedPipe(%s) failed: %lu\n", engine.namedPipeTo, GetLastError());
        return 1;
    }

	engine.hPipeFromEngine = CreateNamedPipeA(
        engine.namedPipeFrom,
        PIPE_ACCESS_INBOUND, // Parent reads from this pipe
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        NULL
    );
    if (engine.hPipeFromEngine == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "engine() error: CreateNamedPipe(%s) failed: %lu\n", engine.namedPipeFrom, GetLastError());
        CloseHandle(engine.hPipeToEngine);
        return 1;
    }
	// Add this: security attributes to make handles inheritable
	SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    // Create client-side handles for the child process
    HANDLE hChildToEngine = CreateFileA(
        engine.namedPipeTo,
        GENERIC_READ,
        0,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hChildToEngine == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "engine() error: CreateFile(%s) failed: %lu\n", engine.namedPipeTo, GetLastError());
        CloseHandle(engine.hPipeToEngine);
        CloseHandle(engine.hPipeFromEngine);
        return 1;
    }

    HANDLE hChildFromEngine = CreateFileA(
        engine.namedPipeFrom,
        GENERIC_WRITE,
        0,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hChildFromEngine == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "engine() error: CreateFile(%s) failed: %lu\n", engine.namedPipeFrom, GetLastError());
        CloseHandle(engine.hPipeToEngine);
        CloseHandle(engine.hPipeFromEngine);
        CloseHandle(hChildToEngine);
        return 1;
    }

    // Set up the child process
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hChildToEngine;
    si.hStdOutput = hChildFromEngine;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    // Launch the chess engine
    char cmdLine[512];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\"", engine.engineName);
    if (!CreateProcessA(
        NULL,
        cmdLine,
        NULL,
        NULL,
        TRUE, // Inherit handles
        0,
        NULL,
        NULL,
        &si,
        &pi
    )) {
        fprintf(stderr, "engine() error: CreateProcess failed: %lu\n", GetLastError());
        CloseHandle(engine.hPipeToEngine);
        CloseHandle(engine.hPipeFromEngine);
        CloseHandle(hChildToEngine);
        CloseHandle(hChildFromEngine);
        return 1;
    }
	engine.hProcess = pi.hProcess;
    // Close child handles in parent
    CloseHandle(hChildToEngine);
    CloseHandle(hChildFromEngine);
    CloseHandle(pi.hThread); // Don't need thread handle
    // Note: Keep pi.hProcess to monitor child process if needed

    // Convert parent pipe handles to FILE* for fgets/fputs compatibility
    int fdToEngine = _open_osfhandle((intptr_t)engine.hPipeToEngine, _O_WRONLY | _O_BINARY);
    if (fdToEngine == -1) {
        fprintf(stderr, "engine() error: _open_osfhandle(toEngine) failed\n");
        CloseHandle(engine.hPipeToEngine);
        CloseHandle(engine.hPipeFromEngine);
        CloseHandle(pi.hProcess);
        return 1;
    }
    engine.toEngine = _fdopen(fdToEngine, "wb");
    if (!engine.toEngine) {
        fprintf(stderr, "engine() error: _fdopen(toEngine) failed\n");
        _close(fdToEngine);
				CloseHandle(engine.hPipeFromEngine);
        CloseHandle(pi.hProcess);
        return 1;
    }

    int fdFromEngine = _open_osfhandle((intptr_t)engine.hPipeFromEngine, _O_RDONLY | _O_BINARY);
    if (fdFromEngine == -1) {
        fprintf(stderr, "engine() error: _open_osfhandle(fromEngine) failed\n");
        fclose(engine.toEngine);
		CloseHandle(engine.hPipeFromEngine);
        CloseHandle(pi.hProcess);
        return 1;
    }
    engine.fromEngine = _fdopen(fdFromEngine, "rb");
    if (!engine.fromEngine) {
        fprintf(stderr, "engine() error: _fdopen(fromEngine) failed\n");
        fclose(engine.toEngine);
        _close(fdFromEngine);
        CloseHandle(pi.hProcess);
        return 1;
    }

    // Ensure text mode for fgets/fputs
    setvbuf(engine.toEngine, NULL, _IONBF, 0); // Unbuffered for timely writes
    setvbuf(engine.fromEngine, NULL, _IONBF, 0); // Unbuffered for timely reads

    //printf("Created pipes: toEngine=%s, fromEngine=%s\n", engine.namedPipeTo, engine.namedPipeFrom);
#else
#include <pthread.h>
    // Create toEngine pipe
    do {
        suffix[0] = '.';
        for (int i = 1; i < 9; i++) {
            suffix[i] = symbols[randomNumber(0, 61)];
        }
        suffix[9] = '\0';
        snprintf(engine.namedPipeTo, MAX_PIPE_NAME_LEN, "%s%s", TO_ENGINE_NAMED_PIPE_PREFIX, suffix);
    } while (mkfifo(engine.namedPipeTo, S_IRUSR | S_IWUSR) == -1);

    // Create fromEngine pipe
    do {
        suffix[0] = '.';
        for (int i = 1; i < 9; i++) {
            suffix[i] = symbols[randomNumber(0, 61)];
        }
        suffix[9] = '\0';
        snprintf(engine.namedPipeFrom, MAX_PIPE_NAME_LEN, "%s%s", FROM_ENGINE_NAMED_PIPE_PREFIX, suffix);
    } while (mkfifo(engine.namedPipeFrom, S_IRUSR | S_IWUSR) == -1);

    pid_t enginePid = fork();
    if (enginePid < 0) {
        fprintf(stderr, "engine() error: fork failed: %s\n", strerror(errno));
        remove(engine.namedPipeTo);
        remove(engine.namedPipeFrom);
        return 1;
    }

    if (enginePid == 0) { // Child
        int toEngine = open(engine.namedPipeTo, O_RDONLY);
        if (toEngine == -1) {
            fprintf(stderr, "engine() child error: open(%s, O_RDONLY): %s\n", engine.namedPipeTo, strerror(errno));
            exit(1);
        }
        int fromEngine = open(engine.namedPipeFrom, O_WRONLY);
        if (fromEngine == -1) {
            fprintf(stderr, "engine() child error: open(%s, O_WRONLY): %s\n", engine.namedPipeFrom, strerror(errno));
            close(toEngine);
            remove(engine.namedPipeTo);
            exit(1);
        }
        dup2(toEngine, STDIN_FILENO);
        close(toEngine);
        dup2(fromEngine, STDOUT_FILENO);
        close(fromEngine);
        if (execlp(engine.engineName, engine.engineName, NULL) < 0) {
            fprintf(stderr, "engine() child error: execlp failed: %s\n", strerror(errno));
            exit(1);
        }
    } else { // Parent
        if ((engine.toEngine = fopen(engine.namedPipeTo, "w")) == NULL) {
            fprintf(stderr, "engine() parent error: fopen(%s, w): %s\n", engine.namedPipeTo, strerror(errno));
            remove(engine.namedPipeTo);
            remove(engine.namedPipeFrom);
            return 1;
        }
        engine.enginePid = (int)enginePid; //so releaseChessEngine can reap and kill it
        if ((engine.fromEngine = fopen(engine.namedPipeFrom, "r")) == NULL) {
            fprintf(stderr, "engine() parent error: fopen(%s, r): %s\n", engine.namedPipeFrom, strerror(errno));
            fclose(engine.toEngine);
            remove(engine.namedPipeTo);
            remove(engine.namedPipeFrom);
            return 1;
        }
        //Unbuffered, so poll() in engineFgets() is authoritative: with stdio buffering,
        //poll could report "nothing to read" while a complete line already sat in the
        //FILE buffer, and the timed read would time out spuriously.
        setvbuf(engine.fromEngine, NULL, _IONBF, 0);
    }
    //printf("Created pipes: toEngine=%s, fromEngine=%s\n", engine.namedPipeTo, engine.namedPipeFrom);
#endif
	return 0;
}

void initChessEngine(Engine& chessEngine, const char * engineName, const long long movetime, const int depth, const int hashSize, const int threadNumber, const char * syzygyPath, const int multiPV, const bool logging, const bool limitStrength, const int elo) {
  //Engine * chessEngine = (struct Engine *)calloc(1, sizeof(struct Engine));
  //Engine * chessEngine = new(Engine);
  chessEngine.movetime = movetime;
  chessEngine.depth = depth;
  int res = engine(chessEngine, engineName);
  if (res) {
    printf("initChessEngine() error: engine(%s) returned %d\n", engineName, res);
    return;
  }
  if (logging) {
	  const char symbols[63] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	  char suffix[10];
	  char logfile[255] = "";
	  bool fileExists = false;
	  do {
			suffix[0] = '.';
			for (int i = 1; i < 9; i++) {
				suffix[i] = symbols[randomNumber(0, 61)];
			}
			suffix[9] = '\0';
	        strcpy(logfile, "engine_log");
			strncat(logfile, suffix, 10);
		    FILE * fp = fopen("logfile", "r");
			if (fp) {
				fileExists = true;
				fclose(fp);
			} else fileExists = false;
		} while(fileExists);
	  chessEngine.logfile = fopen(logfile, "a");
  } else chessEngine.logfile = NULL;
  getOptions(chessEngine);
  int idx = nametoindex(chessEngine, "MultiPV", Spin);
  if (idx >= 0) chessEngine.optionSpin[idx].value = multiPV;
  idx = nametoindex(chessEngine, "Hash", Spin);
  if (idx >= 0) chessEngine.optionSpin[idx].value = hashSize;
  idx = nametoindex(chessEngine, "Threads", Spin);
  if (idx >= 0) chessEngine.optionSpin[idx].value = threadNumber;
  idx = nametoindex(chessEngine, "SyzygyPath", String);
  if (idx >= 0) strncpy(chessEngine.optionString[idx].value, syzygyPath, MAX_UCI_OPTION_STRING_LEN);
  idx = nametoindex(chessEngine, "UCI_Elo", Spin);
  if (idx >= 0) chessEngine.optionSpin[idx].value = elo;
  idx = nametoindex(chessEngine, "UCI_LimitStrength", Check);
  if (idx >= 0) chessEngine.optionCheck[idx].value = limitStrength;
  
  //enable stockfish debug logging
  /*
  idx = nametoindex(chessEngine, "Debug Log File", String);
	buf = malloc(sizeof(struct stat));
  do {
		suffix[0] = '.';
		for (int i = 1; i < 9; i++) {
			suffix[i] = symbols[randomNumber(0, 61)];
		}
		suffix[9] = '\0';
    strcpy(logfile, "stockfish_log");
		strncat(logfile, suffix, 10);
	} while(!stat(logfile, buf));
	free(buf);
  strncpy(chessEngine.optionString[idx].value, logfile, MAX_UCI_OPTION_STRING_LEN);
  */
  setOptions(chessEngine);
  int timeout = 0;
  while (!isReady(chessEngine) && timeout < 3) {
  	if (chessEngine.logfile) fprintf(chessEngine.logfile, "initChessEngine() warning: isReady() returned false\n");
    printf("initChessEngine() warning: isReady() returned false\n");
    //nanosleep(&delay, NULL);
	std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    timeout++;
  }
  if (!isReady(chessEngine)) {
  	if (chessEngine.logfile) fprintf(chessEngine.logfile, "initChessEngine() error: isReady() returned false\n");
  	//free(chessEngine);
    printf("initChessEngine() error: isReady() returned false\n");
  	return;
  }
  //return chessEngine;
}

void releaseChessEngine(Engine& chessEngine) {
	//Ask the engine to leave BEFORE the pipe it listens on is closed. Previously no
	//'quit' was ever sent and the child was never reaped, so every restart left a live
	//multi-threaded engine behind - on an 8 GB machine a few of those are fatal.
	if (chessEngine.toEngine) {
		fprintf(chessEngine.toEngine, "quit\n");
		fflush(chessEngine.toEngine);
	}
	if (chessEngine.logfile) {
		fclose(chessEngine.logfile);
		chessEngine.logfile = nullptr;
	}
	if (chessEngine.toEngine) {
		fclose(chessEngine.toEngine);
		chessEngine.toEngine = nullptr;		
	}
	if (chessEngine.fromEngine) {
		fclose(chessEngine.fromEngine);
		chessEngine.fromEngine = nullptr;
	}
	remove(chessEngine.namedPipeTo);
  remove(chessEngine.namedPipeFrom);
#ifndef _WIN32
	//Reap the child. Give it a moment to act on 'quit', then insist. Without this the
	//process lingers as a zombie at best and a running engine at worst.
	if (chessEngine.enginePid > 0) {
		int status = 0;
		bool reaped = false;
		for (int i = 0; i < 20; ++i) { //up to ~2 s
			pid_t r = waitpid((pid_t)chessEngine.enginePid, &status, WNOHANG);
			if (r == (pid_t)chessEngine.enginePid || (r == -1 && errno == ECHILD)) { reaped = true; break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!reaped) {
			fprintf(stderr, "releaseChessEngine(): engine pid %d did not exit on quit; killing it\n", chessEngine.enginePid);
			kill((pid_t)chessEngine.enginePid, SIGKILL);
			waitpid((pid_t)chessEngine.enginePid, &status, 0);
		}
		chessEngine.enginePid = -1;
	}
#endif
#ifdef _WIN32
	if (chessEngine.hPipeToEngine != INVALID_HANDLE_VALUE) {
		CloseHandle(chessEngine.hPipeToEngine);
		chessEngine.hPipeToEngine = INVALID_HANDLE_VALUE;
	}
	if (chessEngine.hPipeFromEngine != INVALID_HANDLE_VALUE) {
		CloseHandle(chessEngine.hPipeFromEngine);
		chessEngine.hPipeFromEngine = INVALID_HANDLE_VALUE;
	}
	if (chessEngine.hProcess != INVALID_HANDLE_VALUE) {
		CloseHandle(chessEngine.hProcess);
		chessEngine.hProcess = INVALID_HANDLE_VALUE;
	}
#endif
  //delete chessEngine;
  //chessEngine = nullptr;
}

//#ifdef __cplusplus
//}
//#endif
