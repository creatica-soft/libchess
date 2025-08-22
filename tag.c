#pragma warning(disable:4996)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

#ifdef __cplusplus
extern "C" {
#endif

/// <summary>
/// Parses the tag line into tag array indexed by enum Tags
/// </summary>
int strtotag(Tag tag, const char * tagString) {
	char name[MAX_TAG_NAME_LEN], value[MAX_TAG_VALUE_LEN];
	int res = sscanf(tagString, "[%31s \"%89[^]\"]", name, value);
	if (res != 2) {
		printf("strtotag() error: unable to parse the tagString %s\n", tagString);
		return 1;
	}

	size_t sz = strlen(value);
	if (sz >= MAX_TAG_VALUE_LEN) {
		printf("strtotag() error: tag value is too long, it must be shorter than %d\n", MAX_TAG_VALUE_LEN);
		return 1;
	}

	for (int i = 0; i < MAX_NUMBER_OF_TAGS; i++) {
		if (strcmp(tags[i], name) == 0) {
			strncpy(tag[i], value, sz + 1);
			break;
		}
	}
	return 0;
}

int strtoecotag(EcoTag tag, const char * tagString) {
	char name[MAX_ECO_TAG_NAME_LEN], value[MAX_TAG_VALUE_LEN];
	int res = sscanf(tagString, "[%9s \"%89[^]\"]", name, value);
	if (res != 2) {
		printf("strtoecotag() error: unable to parse the tagString %s\n", tagString);
		return 1;
	}

	size_t sz = strlen(value);
	if (sz >= MAX_TAG_VALUE_LEN) {
		printf("strtoecotag() error: tag value is too long, it must be shorter than %d\n", MAX_TAG_VALUE_LEN);
		return 1;
	}

	for (int i = 0; i < MAX_NUMBER_OF_ECO_TAGS; i++) {
		if (strcmp(ecotags[i], name) == 0) {
			strncpy(tag[i], value, sz + 1);
			break;
		}
	}
	return 0;
}
#ifdef __cplusplus
}
#endif
