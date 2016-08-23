#include "conn_work.h"

#include <stdio.h>
#include <assert.h>
#include <string>
#include <limits>
#include "config.h"
#include "memcached_cmd.h"

conn_work::conn_work(const int id, const memdb *db, const server_addr &saddr, double init_send_rate, double send_rate, double ramp_up_speed):
id(id), db(db), saddr(saddr), init_send_rate(init_send_rate), send_rate(send_rate), ramp_up_speed(ramp_up_speed),
hist_latency("hist_latency", 1.0e3),
hist_request_interval("hist_request_interval", 1.0e4),
hist_response_interval("hist_response_interval", 1.0e4) {

	db_idx = 0;

	for (int i = 0; i < cwc_core_end; i++) {
		core_counters[i] = 0.0;
	}
	core_counters[cwc_min_latency] = std::numeric_limits<double>::infinity();
}

void conn_work::make_request(request *r, rand_engine_t *rg) {

	int entry_index = -1;

	if (conf.set_miss) {
		miss_lock.lock();
		if (!missed_key_seeds.empty()) {
			r->cmd = mcm_set;
			entry_index = db->key_seed_to_entry(missed_key_seeds.front());
			missed_key_seeds.pop_front();
		}
		miss_lock.unlock();
	}

	if (entry_index == -1) {
		r->cmd = conf.default_cmd;
		if (conf.set_ratio != 0.0) {
			rand_uniform_real_t dist(0.0, 1.0);
			if (dist(*rg) < conf.set_ratio) {
				r->cmd = mcm_set;
			}
		}
		if (conf.enumerate_items) {
			entry_index = db_idx;
			db_idx = (db_idx + 1) % db->get_dbsize();
		} else {
			entry_index = db->rand_pick_entry(rg);
		}
	}

	db->fill_request(r, entry_index);
}

void conn_work::count_send_timing(double target_start_point, double start_point, double finish_point) {
	assert(target_start_point <= start_point);
	assert(start_point <= finish_point);
	cwc_lock.lock();
	core_counters[cwc_send_delay_sum] += (start_point - target_start_point) / 1.0e3;
	core_counters[cwc_send_duration_sum] += (finish_point - start_point) / 1.0e3;
	cwc_lock.unlock();
}

void conn_work::count_sent(const request &r) {
	cwc_lock.lock();
	switch (r.cmd) {
		case mcm_set:
			core_counters[cwc_sent_set_query]++;
			break;
		case mcm_get:
			core_counters[cwc_sent_get_query]++;
			break;
		default:
			fprintf(stderr, "unknown request cmd\n");
			exit(1);
	}
	hist_request_interval.add_sample(r.send_time);
	cwc_lock.unlock();
}

void conn_work::count_replied(const request &r, const response &resp) {
	cwc_lock.lock();
	switch (resp.err) {
		case mer_set_ok:
			core_counters[cwc_replied_set_query]++;
			break;
		case mer_get_found:
			core_counters[cwc_replied_get_query]++;
			core_counters[cwc_hit_get_query]++;
			break;
		case mer_get_not_found:
			core_counters[cwc_replied_get_query]++;
			break;
		default:
			fprintf(stderr, "unknown request cmd\n");
			exit(1);
	}

	hist_response_interval.add_sample(resp.recv_time);
	hist_latency.add_sample(resp.recv_time, r.send_time);

	double latency = (resp.recv_time - r.send_time) / 1.0e6;

	if (latency <= 0.0) {
		fprintf(stderr, "Oops ... latency <= 0.0: %lf\n", latency);
		exit(1);
	}

	core_counters[cwc_latency_sum] += latency;
	if (latency <= conf.qos) {
		core_counters[cwc_good_qos_query]++;
	}

	if (latency > core_counters[cwc_max_latency]) {
		core_counters[cwc_max_latency] = latency;
	}
	if (latency < core_counters[cwc_min_latency]) {
		core_counters[cwc_min_latency] = latency;
	}

	cwc_lock.unlock();

	if (resp.err == mer_get_not_found && conf.set_miss) {
		miss_lock.lock();
		missed_key_seeds.push_back(r.key_seed);
		miss_lock.unlock();
	}
}

void conn_work::count_udp_timeout() {
	cwc_lock.lock();
	core_counters[cwc_udp_timeout]++;
	cwc_lock.unlock();
}

void conn_work::update_counters() {

	cwc_lock.lock();
	for (int i = 0; i < cwc_core_end; i++) {
		all_counters[i] = core_counters[i];
	}
	core_counters[cwc_max_latency] = 0.0;
	core_counters[cwc_min_latency] = std::numeric_limits<double>::infinity();
	cwc_lock.unlock();

	all_counters[cwc_sent_query] = all_counters[cwc_sent_set_query] + all_counters[cwc_sent_get_query];
	all_counters[cwc_replied_query] = all_counters[cwc_replied_set_query] + all_counters[cwc_replied_get_query];
	all_counters[cwc_retired_query] = all_counters[cwc_replied_query] + all_counters[cwc_udp_timeout];
	all_counters[cwc_outstanding_query] = all_counters[cwc_sent_query] - all_counters[cwc_retired_query];
}

void conn_work::dump_histogram(const char* directory) {
	char filename_prefix_cstr[1024];
	sprintf(filename_prefix_cstr, "%s/sip-%s-sport-%s-cip-%s-cport-%d", directory, saddr.hostname, saddr.port, client_ip, client_port);
	std::string filename_prefix(filename_prefix_cstr);

	cwc_lock.lock();
	hist_latency.dump(filename_prefix + ".latency");
	hist_request_interval.dump(filename_prefix + ".request_interval");
	hist_response_interval.dump(filename_prefix + ".response_interval");
	cwc_lock.unlock();
}
