#ifndef CLOCK_H
#define CLOCK_H

// set starting point to current time 
// (CLOCK_MONOTONIC_RAW time, ignore nano second component)
void init_clock_mono_nsec();

// return nano seconds after staring point
double clock_mono_nsec();

#endif
