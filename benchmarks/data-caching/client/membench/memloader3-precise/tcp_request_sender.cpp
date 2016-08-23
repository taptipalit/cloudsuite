#include "tcp_request_sender.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/uio.h>
#include <errno.h>
#include "clock.h"

tcp_request_sender::tcp_request_sender() {
	send_buf_sz = max_request_size;
	send_buf = new char[send_buf_sz];
}

tcp_request_sender::~tcp_request_sender() {
	delete[] send_buf;
}

void tcp_request_sender::setup(const request &r) {
	progress = 0;
	target = fill_send_buf(r, send_buf, send_buf_sz);
}

bool tcp_request_sender::try_send(double *send_time) {
	while (progress != target) {
		*send_time = clock_mono_nsec();
		int res = send(sock, send_buf + progress, target - progress, MSG_DONTWAIT);
		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return false;
			} else {
				perror("send_data: can't send");
				exit(1);
			}
		}
		progress += res;
	}
	return true;
}
