#ifndef TCP_RESPONSE_RECEIVER_H
#define TCP_RESPONSE_RECEIVER_H

#include <vector>
#include "memcached_cmd.h"

enum tcp_recv_state {
	trs_head,
	trs_body,
	trs_done
};

// A receiver can only be used by one thread at a time.
class tcp_response_receiver {
public:
	int sock;

private:
	char *recv_buf;
	int recv_buf_sz;
	char *buf_head;
	char *buf_tail;
	tcp_recv_state state;
	int skip_target;
	response cur_resp;
	std::vector<response> resp_vec;

public:
	tcp_response_receiver();
	~tcp_response_receiver();

	// Read (nonblockin) gand process at most one buffer of data.
	// Returns empty vector if no complete response is available yet.
	// The returned vector will be overriden on the next call.
	const std::vector<response>& try_receive();

private:
	void reset_recv_buf();
	int recv_some();
	char *get_line();
	bool skip();
	tcp_recv_state handle_head();
	tcp_recv_state handle_body();
	void run_state_machine();
};

#endif
