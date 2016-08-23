#ifndef TCP_REQUEST_SENDER_H
#define TCP_REQUEST_SENDER_H

#include <sys/socket.h>
#include "memcached_cmd.h"

// A sender object can only be used by one thread.
class tcp_request_sender {
public:
	int sock;

private:
	char *send_buf;
	int send_buf_sz;
	int progress;
	int target;

public:
	tcp_request_sender();
	~tcp_request_sender();
	void setup(const request &r);
	bool try_send(double *send_time);
};

#endif
