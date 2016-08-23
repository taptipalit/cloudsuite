#include "udp_response_receiver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include "util.h"
#include "clock.h"
#include "config.h"

udp_response_receiver::udp_response_receiver() {
	recv_buf_sz = sizeof(udp_request_header) + max_response_size;
	recv_buf = new char[recv_buf_sz];
}

udp_response_receiver::~udp_response_receiver() {
	delete[] recv_buf;
}

bool udp_response_receiver::try_receive(response_segment *seg) {

	int dg_size = recvfrom(sock, recv_buf, recv_buf_sz, MSG_DONTWAIT, NULL, NULL);
	double recv_time = clock_mono_nsec();
	if (dg_size <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return false;
		}
		perror("udp receive: can't receive");
		exit(1);
	}

	const int head_sz = sizeof (udp_request_header);
	const int body_sz = dg_size - head_sz;
	char *p = recv_buf;
	udp_request_header *resp_hdr = (udp_request_header*) p;
	p += head_sz;

	seg->udp_id = ntohs(resp_hdr->id);
	seg->cur_segment = ntohs(resp_hdr->seq_no);
	seg->segment_cnt = ntohs(resp_hdr->dgram_cnt);

	if (seg->cur_segment == 0) {
		int i = 0;
		for (; i < body_sz; i++) {
			if (p[i] == '\n') {
				p[i] = '\0';
				break;
			}
		}
		if (i == body_sz) {
			fprintf(stderr, "response header can't fit in first UDP packet\n");
			exit(1);
		}
		parse_response_head(&seg->resp, p);
	}

	seg->resp.recv_time = recv_time;

	return true;
}
