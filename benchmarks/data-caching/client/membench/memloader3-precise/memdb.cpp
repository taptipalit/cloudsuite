#include "memdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

memdb_sample::memdb_sample(const char *filename) {

	FILE *fp;

	if (strcmp(filename, "-") == 0) {
		fp = stdin;
	} else {
		fp = fopen(filename, "r");
		if (fp == NULL) {
			perror("memdb_sample");
			exit(1);
		}
	}

	max_pop_tag = -1;

	while (true) {

		sample_entry en;
		int32_t pop;

		int err = fscanf(fp, "%d %d %d\n", &en.key_size, &en.val_size, &pop);
		if (err == EOF) break;

		char vss[16];
		sprintf(vss, "%d", en.val_size);
		en.vss_size = strlen(vss);

		if (pop <= 0) {
			fprintf(stderr, "memdb_sample: pop <= 0: %d\n", pop);
			exit(1);
		}

		max_pop_tag += pop;
		en.pop_tag = max_pop_tag;

		if (max_pop_tag < 0) {
			fprintf(stderr, "memdb_sample: max_pop_tag overflow: %d\n", max_pop_tag);
			exit(1);
		}

		entries.push_back(en);
	}

	if (fp != stdin) fclose(fp);

	printf("db sample file: %s\n", filename);
	printf("sample size: %lu\n", entries.size());
	printf("max_pop_tag: %d\n", max_pop_tag);
}

memdb::memdb(const memdb_sample *sample, int dbsize, int first_key_seed):
sample(sample), dbsize(dbsize), first_key_seed(first_key_seed) {

	if (dbsize % sample->entries.size() != 0) {
		fprintf(stderr, "dbsize is not a multiple of sample size\n");
		exit(1);
	}

	col_cnt = sample->entries.size();
	row_cnt = dbsize / col_cnt;

}

int memdb::rand_pick_entry(rand_engine_t *rg) const {

	rand_uniform_int_t row_dist(0, row_cnt - 1);
	rand_uniform_int_t point_dist(0, sample->max_pop_tag);

	int row_id, col_id;
	row_id = row_dist(*rg);

	const std::vector<sample_entry> &ses = sample->entries;
	int left, mid, right;
	left = 0;
	right = col_cnt;
	int32_t low, high;
	int32_t point = point_dist(*rg);

	while (left < right) {
		mid = left + (right - left) / 2;
		low = mid == 0 ? 0 : (ses[mid - 1].pop_tag + 1);
		high = ses[mid].pop_tag;
		if (low <= point && point <= high) {
			col_id = mid;
			//printf("point=%d, low=%d, high=%d, row=%d, col=%d\n", point, low, high, row_id, col_id);
			//sleep(1);
			return row_id * col_cnt + col_id;
		} else if (point < low) {
			right = mid;
		} else {
			left = mid + 1;
		}
	}

	assert(false);
	return -1;
}

int memdb::key_seed_to_entry(int key_seed) const {
	return key_seed - first_key_seed;
}

void memdb::fill_request(request *r, int entry_index) const {
	const sample_entry &se = sample->entries[entry_index % col_cnt];
	r->key_seed = first_key_seed + entry_index;
	r->key_size = se.key_size;
	r->val_size = se.val_size;
	r->vss_size = se.vss_size;
}

int memdb::get_dbsize() const {
	return dbsize;
}
