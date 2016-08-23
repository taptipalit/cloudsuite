#ifndef THREAD_UTILS_H
#define THREAD_UTILS_H

#include <thread>

int get_num_of_thread_hosts();

void pin_thread(std::thread *thread, unsigned int thread_host_id);

#endif
