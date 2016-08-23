#include "tcp_response_receiver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>
#include "clock.h"
#include "config.h"

tcp_response_receiver::tcp_response_receiver() {
	recv_buf_sz = conf.mtu * 2;
	recv_buf = new char[recv_buf_sz];
	buf_head = recv_buf;
	buf_tail = recv_buf;
	state = trs_head;
	skip_target = 0;
}

tcp_response_receiver::~tcp_response_receiver() {
	delete[] recv_buf;
}

void tcp_response_receiver::reset_recv_buf() {
	if (buf_head == buf_tail) {
		buf_head = recv_buf;
		buf_tail = recv_buf;
		return;
	}
	int char_cnt = buf_tail - buf_head;
	assert(char_cnt < recv_buf_sz);
	memmove(recv_buf, buf_head, char_cnt);
	buf_head = recv_buf;
	buf_tail = buf_head + char_cnt;
}

int tcp_response_receiver::recv_some() {
	reset_recv_buf();
	// Read as much as buffer can hold.
	int res = recvfrom(sock, buf_tail, recv_buf + recv_buf_sz - buf_tail, MSG_DONTWAIT, NULL, NULL);
	if (res < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			perror("tcp receive: can't receive");
			exit(1);
		}
	}
	buf_tail += res;
	cur_resp.recv_time = clock_mono_nsec();
	return res;
}

char *tcp_response_receiver::get_line() {
	char *line = NULL;
	for (char *p = buf_head; p != buf_tail; p++) {
		if (*p == '\n') {
			*p = '\0';
			line = buf_head;
			buf_head = p + 1;
			return line;
		}
	}
	return NULL;
}

bool tcp_response_receiver::skip() {
	int char_cnt = buf_tail - buf_head;
	if (char_cnt >= skip_target) {
		buf_head += skip_target;
		skip_target = 0;
		return true;
	}
	skip_target -= char_cnt;
	buf_head = buf_tail;
	return false;
}

tcp_recv_state tcp_response_receiver::handle_head() {
	char *line = get_line();
	if (line == NULL) {
		return trs_head;
	}
	parse_response_head(&cur_resp, line);
	if (cur_resp.err == mer_get_found) {
		skip_target = cur_resp.val_size + 7; // 7 is for the trailing "\r\nEND\r\n"
		return trs_body;
	} else {
		return trs_done;
	}
}

tcp_recv_state tcp_response_receiver::handle_body() {
	if (skip()) {
		return trs_done;
	} else {
		return trs_body;
	}
}

void tcp_response_receiver::run_state_machine() {
	while(true) {
		tcp_recv_state old_state = state;
		switch(state) {
		case trs_head:
			state = handle_head();
			break;
		case trs_body:
			state = handle_body();
			break;
		default:
			fprintf(stderr, "Ooops, wrong state.\n");
			exit(1);
		}
		if (state == trs_done) {
			resp_vec.push_back(cur_resp);
			state = trs_head;
		}
		if (state == old_state) {
			break;
		}
	}
}

const std::vector<response>& tcp_response_receiver::try_receive() {
	resp_vec.clear();
	if (recv_some() > 0) {
		run_state_machine();
	}
	return resp_vec;
}
