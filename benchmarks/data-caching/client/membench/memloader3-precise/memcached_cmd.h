#ifndef MEMCACHED_CMD
#define MEMCACHED_CMD

#include <stdint.h>

struct udp_request_header {
	uint16_t id;
	uint16_t seq_no;
	uint16_t dgram_cnt;
	uint16_t reserved;
} __attribute__((packed, aligned(8)));

enum memcmd_t {
	mcm_set,
	mcm_get
};

enum memerr_t {
	mer_set_ok,
	mer_get_not_found,
	mer_get_found
};

class request{
public:
	int key_seed;
	int key_size;
	int val_size;
	int vss_size; // size of value size string
	memcmd_t cmd;
	double send_time; // in ns
};

class response {
public:
	int key_seed;
	int key_size;
	int val_size;
	memerr_t err;
	double recv_time; // in ns
};

static const int max_key_size = 250;
static const int max_val_size = 1 << 20;
static const int max_request_size = max_key_size + max_val_size + 100;
static const int max_response_size = max_key_size + max_val_size + 100;

int fill_send_buf(const request &r, char *buf, int buf_size);
void parse_response_head(response *resp, char *resp_head);
bool request_response_match(const request &r, const response &resp);

#endif
