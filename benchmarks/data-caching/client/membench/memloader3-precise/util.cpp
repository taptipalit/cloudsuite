#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/uio.h>
#include <poll.h>
#include <netdb.h>
#include <string.h>
#include <arpa/inet.h>

void simple_usleep(long long usec) {

	if (usec < 1) return;

	timespec tv;
	tv.tv_sec = usec / 1000000;
	tv.tv_nsec = (usec % 1000000) * 1000;
	nanosleep(&tv, NULL);
}

void checked_complete_read(int fd, void *buf, int size) {

	int done_size = 0;

	while (done_size != size) {
		int cnt = read(fd, (char*) buf + done_size, size - done_size);
		if (cnt <= 0) {
			fprintf(stderr, "checked_complete_read: cnt < 0: %d\n", cnt);
			exit(1);
		}
		done_size += cnt;
	}
}

void checked_complete_write(int fd, const void *buf, int size) {

	int done_size = 0;

	while (done_size != size) {
		int cnt = write(fd, (const char*) buf + done_size, size - done_size);
		if (cnt <= 0) {
			fprintf(stderr, "checked_complete_write: cnt < 0: %d\n", cnt);
			exit(1);
		}
		done_size += cnt;
	}
}

void checked_writev(int fd, const iovec *iovecs, int vcnt, int size) {
	int cnt = writev(fd, iovecs, vcnt);
	if (cnt != size) {
		fprintf(stderr, "checked_writev: cnt != size: %d, %d\n", cnt, size);
		exit(1);
	}
}

int ppoll_wait(int fd, int events, long long utimeout) {
	pollfd pfd;
	pfd.fd = fd;
	pfd.events = events;
	int err;
	if (utimeout < 0) {
		err = ppoll(&pfd, 1, NULL, NULL);
	} else {
		timespec ts;
		ts.tv_sec = utimeout / 1000000;
		ts.tv_nsec = (utimeout % 1000000) * 1000;
		err = ppoll(&pfd, 1, &ts, NULL);
	}

	if (err < 0) {
		perror("ppoll failed");
		exit(1);
	}

	return pfd.revents;
}

void get_sockaddr(sockaddr *addr, const char *host, const char *port, int sock_type) {
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_INET;
	hints.ai_socktype = sock_type;
	hints.ai_protocol = 0;
	addrinfo *res;

	if (getaddrinfo(host, port, &hints, &res) != 0) {
		perror("can't find server");
		exit(1);
	}

	*addr = *(res->ai_addr);

	freeaddrinfo(res);
}

void bind_port(int fd, int port, int sock_type) {
	char port_buf[16];
	sprintf(port_buf, "%d", port);
	sockaddr addr;
	get_sockaddr(&addr, NULL, port_buf, sock_type);
	int ret = bind(fd, &addr, sizeof (addr));
	if (ret < 0) {
		perror("bind_port");
		exit(1);
	}
}

int get_socket_port(int sock) {
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	if (getsockname(sock, (struct sockaddr *)&sin, &len) == -1) {
		perror("getsockname");
	}
	return ntohs(sin.sin_port);
}

void get_socket_ip(int sock, char *buf, int bufsz) {
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	if (getsockname(sock, (struct sockaddr *)&sin, &len) == -1) {
		perror("getsockname");
	}
	
	inet_ntop(AF_INET, &sin.sin_addr, buf, bufsz);
}

