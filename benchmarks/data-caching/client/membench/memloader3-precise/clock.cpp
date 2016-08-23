#include <time.h>
#include "clock.h"

time_t start_point_sec;

void init_clock_mono_nsec() {
	timespec start_point;
	clock_gettime(CLOCK_MONOTONIC_RAW, &start_point);
	start_point_sec = start_point.tv_sec;
}

double clock_mono_nsec() {
	timespec now;
	clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	double secs = difftime(now.tv_sec, start_point_sec);
	double nsecs = (double) now.tv_nsec;
	return secs * 1.0e9 + nsecs;
}
