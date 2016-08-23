#include "memcached_cmd.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "util.h"


static const char *hexa_table = "0123456789ABCDEF";

template<typename NT> void number_to_hexas(NT number, char *hexas) {
	for (int i = sizeof(NT) * 2 - 1; i >= 0; i--) {
		hexas[i] = hexa_table[number & 0x0f];
		number >>= 4;
	}
}

static void fill_key(char *key, int key_seed, int key_size) {
	const int min_key_size = sizeof(key_seed) * 2;
	if (key_size < min_key_size) {
		fprintf(stderr, "key_seed2chars: key_size < min_key_size: %d, %d\n", key_size, min_key_size);
		exit(1);
	}
	const int pad_size = key_size - min_key_size;
	memset(key, 'K', pad_size);
	key += pad_size;
	number_to_hexas(key_seed, key);
}

static int extract_key_seed(char *key, int key_size) {
	int key_seed_str_size = sizeof(int) * 2;
	char *key_seed_str = key + key_size - key_seed_str_size;
	char *p = NULL;
	for (p = key_seed_str; *p == '0'; p++)
		;
	if (*p == '\0') {
		return 0;
	} else {
		return strtol(p, NULL, 16);
	}
}

static void fill_val(char *val, int key_seed, int val_size) {
	const int key_seed_str_size = sizeof(int) * 2;
	const int meta_size = 1/*|*/ + 16/*thread id*/ + 1/*|*/ + 16/*rdtsc*/ + 1/*|*/;
	const int min_val_size = meta_size + key_seed_str_size + 1/*|*/;
	if (val_size < min_val_size) {
		fprintf(stderr, "sprint_val: val_size < min_val_size: %d, %d\n", val_size, min_val_size);
		exit(1);
	}
	char *p = val;
	*p++ = '|';
	number_to_hexas((uint64_t) pthread_self(), p); p += 16;
	*p++ = '|';
	number_to_hexas((uint64_t) rdtsc(), p); p += 16;
	*p++ = '|';
	const int pad_size = (val_size - meta_size) % (key_seed_str_size + 1);
	if (pad_size > 0) {
		memset(p, 'V', pad_size - 1); p += pad_size - 1;
		*p++ = '|';
	}
	for (int i = 0; i < (val_size - meta_size) / (key_seed_str_size + 1); i++) {
		number_to_hexas(key_seed, p); p += key_seed_str_size;
		*p++ = '|';
	}
}

static int fill_set(const request &r, char *buf, int buf_size) {

	assert(r.key_size + r.val_size + 30 <= buf_size);

	char *p = buf;
	memcpy(p, "set ", 4);
	p += 4;
	fill_key(p, r.key_seed, r.key_size);
	p += r.key_size;
	memcpy(p, " 0 0 ", 5);
	p += 5;
	sprintf(p, "%d\r\n", r.val_size);
	p += r.vss_size + 2;
	fill_val(p, r.key_seed, r.val_size);
	p += r.val_size;
	memcpy(p, "\r\n", 2);
	p += 2;

	return p - buf;
}

static int fill_get(const request &r, char *buf, int buf_size) {

	assert(r.key_size + 30 <= buf_size);

	char *p = buf;
	memcpy(p, "get ", 4);
	p += 4;
	fill_key(p, r.key_seed, r.key_size);
	p += r.key_size;
	memcpy(p, "\r\n", 2);
	p += 2;

	return p - buf;
}

int fill_send_buf(const request &r, char *buf, int buf_size) {
	switch (r.cmd) {
		case mcm_set:
			return fill_set(r, buf, buf_size);
		case mcm_get:
			return fill_get(r, buf, buf_size);
		default:
			fprintf(stderr, "tcp send: unknown op: %d\n", r.cmd);
			exit(1);
	}
	return -1;
}

void parse_response_head(response *resp, char *resp_head) {
	char *p = resp_head;
	char *tok_context;
	const char *delim = " \r";

	char *type = strtok_r(p, delim, &tok_context);

	if (strcmp(type, "STORED") == 0) { // set ok
		resp->err = mer_set_ok;
	} else if (strcmp(type, "END") == 0) { // get not found
		resp->err = mer_get_not_found;
	} else if (strcmp(type, "VALUE") == 0) { // get found
		resp->err = mer_get_found;
		char *key = strtok_r(NULL, delim, &tok_context);
		resp->key_size = strlen(key);
		resp->key_seed = extract_key_seed(key, resp->key_size);
		strtok_r(NULL, delim, &tok_context); // flags
		char *value_size_str = strtok_r(NULL, delim, &tok_context); // bytes
		resp->val_size = atoi(value_size_str);
	} else {
		fprintf(stderr, "parse_response_head: unknown type: %s\n", type);
		exit(1);
	}
}

bool request_response_match(const request &r, const response &resp) {
	if (r.cmd == mcm_get) {
		if (resp.err == mer_get_found) {
			if (r.key_seed != resp.key_seed || r.key_size != resp.key_size || r.val_size != resp.val_size) {
				fprintf(stderr, "Oooops, wrong GET hit:%x %d %d\n%x %d %d\n",
					r.key_seed, r.key_size, r.val_size,
					resp.key_seed, resp.key_size, resp.val_size);
				return false;
			}
		} else if (resp.err == mer_get_not_found) {
			;
		} else {
			fprintf(stderr, "Oooops, non-GET response for GET request: %d\n", resp.err);
			return false;
		}
	} else if (r.cmd == mcm_set) {
		if (resp.err != mer_set_ok) {
			fprintf(stderr, "Oooops, non-SET response for SET request: %d\n", resp.err);
			return false;
		}
	}
	return true;
}
