#ifndef UDP_REQUEST_SENDER_H
#define UDP_REQUEST_SENDER_H

#include <sys/socket.h>
#include "memcached_cmd.h"

// A sender object can only be used by one thread.
class udp_request_sender {
public:
	int sock;
	sockaddr saddr;

private:
	char *send_buf;
	int send_buf_sz;
	int segment_sz;
	int udp_id;
	int segment_cnt;
	int cur_segment;
	int last_segment_sz;

public:
	udp_request_sender();
	~udp_request_sender();
	void setup(int udp_id, const request &r);
	bool try_send(double *send_time);

private:
	void fill_header(udp_request_header* h);
};

#endif
