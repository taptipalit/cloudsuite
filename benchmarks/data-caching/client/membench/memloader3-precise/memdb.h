#ifndef MEMDB_H
#define MEMDB_H

#include <stdlib.h>
#include <vector>
#include "randnum.h"
#include "memcached_cmd.h"

class sample_entry {
public:
	int32_t pop_tag;
	int key_size;
	int val_size;
	int vss_size; // size of value size string
};

class memdb_sample {
public:
	std::vector<sample_entry> entries;
	int32_t max_pop_tag;

public:
	memdb_sample(const char *sample_file);
};

class memdb {
private:
	const memdb_sample *sample;
	int dbsize;
	int first_key_seed;
	int col_cnt;
	int row_cnt;

public:
	memdb(const memdb_sample *sample, int dbsize, int first_key_seed);
	int rand_pick_entry(rand_engine_t *rg) const;
	int key_seed_to_entry(int key_seed) const;
	void fill_request(request *r, int entry_index) const;
	int get_dbsize() const;
};

#endif
