#ifndef UTIL_H
#define UTIL_H

#include <sys/socket.h>
#include <sys/types.h>

static inline unsigned long long rdtsc(void)
{
	unsigned hi, lo;
	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)lo)|(((unsigned long long)hi)<<32);
}

/* Will only sleep when usec is greater than 0. */
void simple_usleep(long long usec);

/* 
 * Will call read repeatedly until 'size' bytes are read.
 * Will exit program if this is impossible (e.g., due to not enough data in file).
 */
void checked_complete_read(int fd, void *buf, int size);

/* 
 * Will call write repeatedly until 'size' bytes are writen.
 * Will exit program if this is impossible (e.g., due to file size limit.).
 */
void checked_complete_write(int fd, const void *buf, int size);

/* Will call writev ONCE and check if 'size' bytes are written. If not, will exit the program. */
void checked_writev(int fd, const iovec *iovecs, int vcnt, int size);

/* If utimeout == 0, return immediately. If utimeout < 0, wait indefinitely. */
int ppoll_wait(int fd, int events, long long utimeout);

void get_sockaddr(sockaddr *addr, const char *host, const char *port, int sock_type);

void bind_port(int fd, int port, int sock_type);

int get_socket_port(int sock);

void get_socket_ip(int sock, char *buf, int bufsz);

#endif
