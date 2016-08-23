#include <sched.h>
#include "thread_utils.h"

static int thread_host_id_to_cpu_id(unsigned int thread_host_id) {

	int err;
	cpu_set_t av_cpu_mask;

	err = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &av_cpu_mask);
	if (err != 0) {
		fprintf(stderr, "thread_host_id_to_cpu_id: can't get affinity mask.\n");
		exit(1);
	}

	int av_cpu_cnt = CPU_COUNT(&av_cpu_mask);

	if ((int) thread_host_id >= av_cpu_cnt) {
		fprintf(stderr, "thread_host_id_to_cpu_id: thread_host_id >= av_cpu_cnt: %u, %d.\n", thread_host_id, av_cpu_cnt);
		exit(1);
	}

	unsigned int id_i = 0;
	for (int bit_i = 0; bit_i < CPU_SETSIZE; bit_i++) {
		if (CPU_ISSET(bit_i, &av_cpu_mask)) {
			if (id_i == thread_host_id) return bit_i;
			id_i++;
		}
	}

	return -1;
}

int get_num_of_thread_hosts() {

	int err;
	cpu_set_t av_cpu_mask;

	err = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &av_cpu_mask);
	if (err != 0) {
		fprintf(stderr, "ptu_get_num_of_thread_hosts: can't get affinity mask.\n");
		exit(1);
	}

	return CPU_COUNT(&av_cpu_mask);

}

void pin_thread(std::thread *thread, unsigned int thread_host_id) {

	int err;
	pthread_t tid = thread->native_handle();
	int cpu_id = thread_host_id_to_cpu_id(thread_host_id);
	cpu_set_t cpu_mask;

	CPU_ZERO(&cpu_mask);
	CPU_SET(cpu_id, &cpu_mask);
	err = pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cpu_mask);
	if (err == 0) {
		// printf("thread %lx pinned to cpu: %d\n", tid, cpu_id);
	} else {
		fprintf(stderr, "ptu_pin_thread: failed to pin thread %lx\n", tid);
		exit(1);
	}
}
