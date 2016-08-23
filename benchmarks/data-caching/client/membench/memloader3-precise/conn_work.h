#ifndef CONN_WORK_H
#define CONN_WORK_H

#include <list>
#include <mutex>
#include <sys/socket.h>
#include <atomic>
#include "config.h"
#include "memdb.h"
#include "randnum.h"
#include "memcached_cmd.h"
#include "histogram.h"

#define IP_BUF_SZ 16

enum cwc_names {
	cwc_sent_set_query,
	cwc_sent_get_query,
	cwc_replied_set_query,
	cwc_replied_get_query,
	cwc_hit_get_query,
	cwc_good_qos_query,
	cwc_latency_sum,
	cwc_max_latency,
	cwc_min_latency,
	cwc_send_delay_sum,
	cwc_send_duration_sum,
	cwc_udp_timeout,
	cwc_core_end,
	// derived counters
	cwc_sent_query,
	cwc_replied_query,
	cwc_retired_query,
	cwc_outstanding_query,
	cwc_end
};

class conn_work {
public:
	const int id;
	const memdb * const db;
	const server_addr saddr;
	const double init_send_rate;
	const double send_rate;
	const double ramp_up_speed; // unit is rate increament per second
	double all_counters[cwc_end];

	int client_port;
	char client_ip[IP_BUF_SZ];

private:
	std::mutex cwc_lock;
	std::mutex miss_lock;

	int db_idx; // for enum work
	std::list<int> missed_key_seeds;

	double core_counters[cwc_core_end];

	time_diff_histogram hist_latency;
	interval_histogram hist_request_interval;
	interval_histogram hist_response_interval;

public:
	// If send_rate is 0.0, it means infinite, and requests will be sent as fast as possible (conf.max_outstanding is still effective).
	conn_work(int id, const memdb *db, const server_addr &saddr, double init_send_rate, double send_rate, double ramp_up_speed);

	void make_request(request *r, rand_engine_t *rg);
	void count_send_timing(double target_start_point, double start_point, double finish_point);
	void count_sent(const request &r);
	void count_replied(const request &r, const response &resp);
	void count_udp_timeout();
	void update_counters();

	void dump_histogram(const char *directory);
};

#endif
