#include "udp_request_sender.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <algorithm>
#include "util.h"
#include "clock.h"
#include "config.h"

udp_request_sender::udp_request_sender() {
	send_buf_sz = sizeof(udp_request_header) + max_request_size;
	send_buf = new char[send_buf_sz];
	const int max_ip_packet_sz = (1 << 16) - 1;
	// 100 includes various headers (ip, udp, and memcached-udp)
	segment_sz = std::min(conf.mtu, max_ip_packet_sz) - 100;
}

udp_request_sender::~udp_request_sender() {
	delete[] send_buf;
}

void udp_request_sender::fill_header(udp_request_header* h) {
	if (udp_id > 0xffff || udp_id < 0) {
		fprintf(stderr, "udp_id out of range: %d\n", udp_id);
		exit(1);
	}
	h->id = htons(udp_id);
	h->seq_no = htons(cur_segment);
	h->dgram_cnt = htons(segment_cnt);
	h->reserved = 0;
}

void udp_request_sender::setup(int udp_id, const request &r) {
	int hsz = sizeof (udp_request_header);
	int request_sz = fill_send_buf(r, send_buf + hsz, send_buf_sz - hsz);
	this->udp_id = udp_id;
	segment_cnt = (request_sz + segment_sz - 1) / segment_sz; // round up
	cur_segment = 0;
	last_segment_sz = request_sz % segment_sz;
	if (last_segment_sz == 0) {
		last_segment_sz = segment_sz;
	}
}

bool udp_request_sender::try_send(double *send_time) {
	int hsz = sizeof (udp_request_header);
	while (cur_segment < segment_cnt) {
		// Note that the header of the current packet overlaps with
		// the tail of the previous packet.
		char *packet = send_buf + segment_sz * cur_segment;
		fill_header((udp_request_header*) packet);
		int bsz = (cur_segment == segment_cnt - 1) ? last_segment_sz : segment_sz;
		int packet_sz = hsz + bsz;
		*send_time = clock_mono_nsec();
		int res = sendto(sock, packet, packet_sz, MSG_DONTWAIT, &saddr, sizeof(saddr));
		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return false;
			}
			perror("udp try_send: can't sendto");
			exit(1);
		}
		if (res != packet_sz) {
			fprintf(stderr, "udp try_send: send incomplete: %d %d\n", res, packet_sz);
			exit(1);
		}
		cur_segment++;
	}
	return true;
}
