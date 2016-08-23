#ifndef UDP_RESPONSE_RECEIVER_H
#define UDP_RESPONSE_RECEIVER_H

#include "memcached_cmd.h"

class response_segment {
public:
	int udp_id;
	int cur_segment;
	int segment_cnt;
	response resp;
};

// A receiver can only be used by one thread at a time.
class udp_response_receiver {
public:
	int sock;

private:
	char *recv_buf;
	int recv_buf_sz;

public:
	udp_response_receiver();
	~udp_response_receiver();
	// Nonblocking, returns false if no response segment is available yet, true otherwise.
	bool try_receive(response_segment *seg);
};

#endif
