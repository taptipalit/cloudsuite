#include <sys/types.h>
#include <sys/socket.h>
#include <math.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <list>
#include <algorithm>
#include <thread>
#include <assert.h>
#include <limits>
#include "thread_utils.h"
#include "conn_work.h"
#include "tcp_conn_worker.h"
#include "udp_conn_worker.h"
#include "memcached_cmd.h"
#include "memdb.h"
#include "config.h"
#include "clock.h"

config conf;
controller control;
static int conn_cnt;
static conn_work **conn_works;

static void init_conf() {
	conf.db_sample_file = "-";
	conf.db_size = 5000;

	conf.mirror = false;

	conf.vclients = 1;

	conf.load = 100000.0;
	conf.qos = 1.0;
	conf.udp_timeout = 10000.0;

	conf.udp = false;
	conf.nagles = false;
	conf.default_cmd = mcm_get;
	conf.enumerate_items = false;
	conf.set_miss = false;

	conf.preload = false;

	conf.base_port = 0;

	conf.set_ratio = 0.0;

	conf.per_connection_work = 0;

	conf.histogram_head = 0;
	conf.histogram_body = 0;

	conf.send_traffic_shape.shape = traffic_shape::UNIFORM;
	conf.send_traffic_shape.param = 0.1;

	conf.busy_loop_receive = false;
	conf.receive_burst = 10;

	conf.connect_speed = 50.0; // new connection per second
	conf.connection_init_load = 10.0;
	conf.connection_ramp_up_speed = 100.0; // per connection load increament per second

	conf.mtu = 1500;
}

// c = a - b
static void counters_subtract(double *a, double *b, double *c) {
	for (int i = 0; i < cwc_end; i++) {
		c[i] = a[i] - b[i];
	}
}

// c = a + b
static void counters_plus(double *a, double *b, double *c) {
	for (int i = 0; i < cwc_end; i++) {
		c[i] = a[i] + b[i];
	}
}

static void update_counters() {
	for (int i = 0; i < conn_cnt; i++) {
		conn_works[i]->update_counters();
	}
}

static void sum_counters(double *sums) {
	for (int i = 0; i < cwc_end; i++) {
		sums[i] = 0;
	}
	for (int i = 0; i < conn_cnt; i++) {
		counters_plus(sums, conn_works[i]->all_counters, sums);
	}
}

static void print_stats_summary(double *d, double t) {
	printf("qos %.3f load %.0f send_rate %.0f reply_rate %.0f avg_lat %.3fms avg_sdelay %.1fus avg_sdura %.1fus hit_ratio %.3f get_ratio %.3f set_ratio %.3f udp_timeout %.0f",
		d[cwc_good_qos_query] / d[cwc_retired_query] * 100.0,
		conf.load,
		d[cwc_sent_query] / t,
		d[cwc_replied_query] / t,
		d[cwc_latency_sum] / d[cwc_replied_query],
		d[cwc_send_delay_sum] / d[cwc_sent_query],
		d[cwc_send_duration_sum] / d[cwc_sent_query],
		d[cwc_hit_get_query] / d[cwc_replied_get_query],
		d[cwc_sent_get_query] / d[cwc_sent_query],
		d[cwc_sent_set_query] / d[cwc_sent_query],
		d[cwc_udp_timeout]);
}

static void print_qlen_summary() {

	double cq_max = 0.0;
	double cq_min = std::numeric_limits<double>::infinity();
	double cq_sum = 0.0;

	for (int i = 0; i < conn_cnt; i++) {

		conn_work *work = conn_works[i];
		double qlen = work->all_counters[cwc_outstanding_query];

		cq_sum += qlen;
		if (qlen > cq_max) cq_max = qlen;
		if (qlen < cq_min) cq_min = qlen;
	}

	printf("os_sum %.0f os_max %.0f os_min %.0f os_avg %.0f", cq_sum, cq_max, cq_min, cq_sum / conn_cnt);

	double max_lat = 0.0;
	double min_lat = std::numeric_limits<double>::infinity();

	for (int i = 0; i < conn_cnt; i++) {

		conn_work *work = conn_works[i];
		double cw_max_lat = work->all_counters[cwc_max_latency];
		double cw_min_lat = work->all_counters[cwc_min_latency];

		if (cw_max_lat > max_lat) max_lat = cw_max_lat;
		if (cw_min_lat < min_lat) min_lat = cw_min_lat;
	}

	printf(" max_lat %.3fms min_lat %.3fms", max_lat, min_lat);
}

static void report(double *deltas, double nsec_duration) {
	double duration = nsec_duration / 1.0e9;
	print_stats_summary(deltas, duration);
	printf(" ");
	print_qlen_summary();
	printf("\n");
}

static bool preload_done() {
	for (int i = 0; i < conn_cnt; i++) {
		conn_work *work = conn_works[i];
		double query_done = work->all_counters[cwc_replied_query];
		if (query_done < work->db->get_dbsize()) {
			return false;
		}
	}
	return true;
}

static bool per_connection_work_done() {
	for (int i = 0; i < conn_cnt; i++) {
		conn_work *work = conn_works[i];
		double query_done = work->all_counters[cwc_replied_query];
		if (query_done < conf.per_connection_work) {
			return false;
		}
	}
	return true;
}

static void do_work_round(const work_round &rd) {
	double inits[cwc_end], olds[cwc_end], news[cwc_end], deltas[cwc_end];
	double init_tv, old_tv, new_tv;

	update_counters();
	sum_counters(inits);
	init_tv = clock_mono_nsec();

	for (int i = 0; i < rd.iter_cnt || rd.iter_cnt == 0; i++) {

		update_counters();
		sum_counters(olds);
		old_tv = clock_mono_nsec();

		sleep(rd.interval);

		update_counters();
		sum_counters(news);
		new_tv = clock_mono_nsec();

		if (rd.discrete) {
			counters_subtract(news, olds, deltas);
			printf("D: ");
			report(deltas, new_tv - old_tv);
		}
		if (rd.accumulate) {
			counters_subtract(news, inits, deltas);
			printf("A: ");
			report(deltas, new_tv - init_tv);
		}
		fflush(stdout);

		if (conf.preload && preload_done()) {
			printf("===preload finished, break round===\n");
			return;
		}

		if (conf.per_connection_work > 0 && per_connection_work_done()) {
			printf("===per_connection_work finished, break round===\n");
			return;
		}
	}
}

static void do_work() {
	printf("===work started===\n");
	for (int rid = 0; rid < (int) conf.work_rounds.size(); rid++) {
		work_round &rd = conf.work_rounds[rid];
		printf("===round %d started[iteratrions=%d, interval=%d]===\n", rid, rd.iter_cnt, rd.interval);
		do_work_round(rd);
		printf("===round %d finished===\n", rid);
	}
	printf("===work finished===\n");
}

static int parse_server_spec(int argc, char **argv) {

	server_record sr;
	int i = 0;

	for (i = 0; i < argc; i += 2) {
		if (argv[i][0] == '-') break;
		server_addr sa;
		sa.hostname = argv[i];
		sa.port = argv[i+1];
		sr.addrs.push_back(sa);
	}

	conf.servers.push_back(sr);
	return i;
}

static int parse_command_spec(int argc, char **argv) {
	int i = 0;
	for (i = 0; i < argc; i++) {
		const char *s = argv[i];
		if (strcmp(s, "set") == 0) {
			conf.default_cmd = mcm_set;
		} else if (strcmp(s, "enum") == 0) {
			conf.enumerate_items = true;
		} else if (strcmp(s, "set-miss") == 0) {
			conf.set_miss = true;
		} else if (s[0] == '-') {
			break;
		} else {
			fprintf(stderr, "parse_command_spec: unknown token: %s\n", s);
			exit(1);
		}
	}
	return i;
}

static int parse_round_spec(int argc, char **argv) {
	work_round rd;
	int i = 0;

	rd.discrete = true;
	rd.accumulate = false;

	if (strcmp(argv[i], "acc") == 0) {
		rd.discrete = false;
		rd.accumulate = true;
		i++;
	} else if (strcmp(argv[i], "all") == 0) {
		rd.discrete = true;
		rd.accumulate = true;
		i++;
	}

	rd.iter_cnt = atof(argv[i++]);
	rd.interval = atof(argv[i++]);

	conf.work_rounds.push_back(rd);

	return i;
}

static int parse_send_traffic_shape(int argc, char **argv) {
	int i = 0;
	if (strcmp(argv[i], "uniform") == 0) {
		conf.send_traffic_shape.shape = traffic_shape::UNIFORM;
		i++;
		conf.send_traffic_shape.param = atof(argv[i]);
		i++;
	} else if (strcmp(argv[i], "normal") == 0) {
		conf.send_traffic_shape.shape = traffic_shape::NORMAL;
		i++;
		conf.send_traffic_shape.param = atof(argv[i]);
		i++;
	} else if (strcmp(argv[i], "peaks") == 0) {
		conf.send_traffic_shape.shape = traffic_shape::PEAKS;
		i++;
		conf.send_traffic_shape.param = atof(argv[i]);
		i++;
	} else if (strcmp(argv[i], "gamma") == 0) {
		conf.send_traffic_shape.shape = traffic_shape::GAMMA;
		i++;
		conf.send_traffic_shape.param = atof(argv[i]);
		i++;
	} else if (strcmp(argv[i], "exponential") == 0) {
		conf.send_traffic_shape.shape = traffic_shape::EXPONENTIAL;
		i++;
		conf.send_traffic_shape.param = atof(argv[i]);
		i++;
	} 
	return i;
}

static void parse_arguments(int argc, char **argv) {

	if (argc == 0) return;

	for (int i = 0; i < argc; ) {
		const char *key = argv[i++];
		if (strcmp(key, "--db") == 0) {
			conf.db_sample_file = argv[i++];
			conf.db_size = atof(argv[i++]);
		} else if (strcmp(key, "--server") == 0) {
			i += parse_server_spec(argc - i, argv + i);
		} else if (strcmp(key, "--mirror") == 0) {
			conf.mirror = true;
		} else if (strcmp(key, "--vclients") == 0) {
			conf.vclients = atof(argv[i++]);
		} else if (strcmp(key, "--load") == 0) {
			conf.load = atof(argv[i++]);
		} else if (strcmp(key, "--qos") == 0) {
			conf.qos = atof(argv[i++]);
		} else if (strcmp(key, "--udp-timeout") == 0) {
			conf.udp_timeout = atof(argv[i++]);
		} else if (strcmp(key, "--round") == 0) {
			i += parse_round_spec(argc - i, argv + i);
		} else if (strcmp(key, "--udp") == 0) {
			conf.udp = true;
		} else if (strcmp(key, "--nagles") == 0) {
			conf.nagles = true;
		} else if (strcmp(key, "--command") == 0) {
			i += parse_command_spec(argc - i, argv + i);
		} else if (strcmp(key, "--preload") == 0) {
			conf.preload = true;
		} else if (strcmp(key, "--base-port") == 0) {
			conf.base_port = atof(argv[i++]);
		} else if (strcmp(key, "--set-ratio") == 0) {
			conf.set_ratio = atof(argv[i++]);
		} else if (strcmp(key, "--per-connection-work") == 0) {
			conf.per_connection_work = atof(argv[i++]);
		} else if (strcmp(key, "--histogram") == 0) {
			conf.histogram_head = atof(argv[i++]);
			conf.histogram_body = atof(argv[i++]);
		} else if (strcmp(key, "--send-traffic-shape") == 0) {
			i += parse_send_traffic_shape(argc - i, argv + i);
		} else if (strcmp(key, "--busy-loop-receive") == 0) {
			conf.busy_loop_receive = true;
		} else if (strcmp(key, "--receive-burst") == 0) {
			conf.receive_burst = atof(argv[i++]);
		} else if (strcmp(key, "--connect-speed") == 0) {
			conf.connect_speed = atof(argv[i++]);
		} else if (strcmp(key, "--connection-init-load") == 0) {
			conf.connection_init_load = atof(argv[i++]);
		} else if (strcmp(key, "--connection-ramp-up-speed") == 0) {
			conf.connection_ramp_up_speed = atof(argv[i++]);
		} else if (strcmp(key, "--mtu") == 0) {
			conf.mtu = atof(argv[i++]);
		} else {
			fprintf(stderr, "parse_arguments: unknown key: %s\n", key);
			exit(1);
		}
	}

	if (conf.preload) {
		conf.udp = false;
		conf.default_cmd = mcm_set;
		conf.enumerate_items = true;
		if (conf.mirror) {
			conf.vclients = conf.servers.size();
		} else { // shard
			conf.vclients = 1;
		}
		conf.work_rounds.clear();
		conf.per_connection_work = 0;
	}

	if (conf.per_connection_work > 0) {
		conf.work_rounds.clear();
		work_round rd;
		rd.discrete = true;
		rd.accumulate = true;
		rd.iter_cnt = 0;
		rd.interval = 2;
		conf.work_rounds.push_back(rd);
	}

	if (conf.work_rounds.size() == 0) {
		work_round rd;
		rd.discrete = true;
		rd.accumulate = false;
		rd.iter_cnt = 0;
		rd.interval = 5;
		conf.work_rounds.push_back(rd);
	}

	if (conf.mirror) {
		if (conf.vclients % conf.servers.size() != 0) {
			fprintf(stderr, "can't divide clients evenly to mirror-servers\n");
			exit(1);
		}
	} else {
		if (conf.db_size % conf.servers.size() != 0) {
			fprintf(stderr, "can't divide db evenly to shard-servers\n");
			exit(1);
		}
	}
}

static server_addr pick_saddr(server_record *srec) {
	std::list<server_addr> *addrs = &srec->addrs;
	addrs->push_back(addrs->front());
	addrs->pop_front();
	return addrs->front();
}

int main(int argc, char **argv) {

	init_clock_mono_nsec();
	init_conf();

	parse_arguments(argc - 1, argv + 1);

	if (conf.mirror) {
		conn_cnt = conf.vclients;
	} else { // shard
		conn_cnt = conf.vclients * conf.servers.size();
	}

	printf("number of connections: %d\n", conn_cnt);

	memdb_sample *sample = new memdb_sample(conf.db_sample_file);
	if (conf.mirror) {
		memdb *db = new memdb(sample, conf.db_size, 0);
		for (int i = 0; i < (int) conf.servers.size(); i++) {
			conf.servers[i].db = db;
		}
	} else { // shard
		int shard_size = conf.db_size / conf.servers.size();
		for (int i = 0; i < (int) conf.servers.size(); i++) {
			conf.servers[i].db = new memdb(sample, shard_size, shard_size * i);
		}
	}

	conn_works = new conn_work*[conn_cnt];
	double avg_load = conf.load / (double) conn_cnt;
	if (conf.preload) {
		conf.connection_init_load = avg_load;
	}
	assert(avg_load >= conf.connection_init_load);
	for (int cid = 0; cid < conn_cnt; cid++) {
		int sid = cid % conf.servers.size();
		server_record *sr = &conf.servers[sid];
		conn_work *work = new conn_work(cid, sr->db, pick_saddr(sr), conf.connection_init_load, avg_load, conf.connection_ramp_up_speed);
		conn_works[cid] = work;
	}

	int thread_host_cnt = get_num_of_thread_hosts();

	assert(thread_host_cnt % 2 == 0);
	int work_list_cnt = std::min(conn_cnt, thread_host_cnt / 2);
	std::list<conn_work*> *work_lists = new std::list<conn_work*>[work_list_cnt];
	for (int cid = 0; cid < conn_cnt; cid++) {
		int lid = cid % work_list_cnt;
		work_lists[lid].push_back(conn_works[cid]);
	}
	double worker_connect_speed = conf.connect_speed / work_list_cnt;
	for (int lid = 0; lid < work_list_cnt; lid++) {
		int send_thost_id = lid * 2;
		int recv_thost_id = lid * 2 + 1;
		if (conf.udp) {
			udp_conn_worker *worker = new udp_conn_worker(work_lists[lid], worker_connect_speed);
			std::thread send_thread(&udp_conn_worker::send_run, worker);
			pin_thread(&send_thread, send_thost_id);
			send_thread.detach();
			std::thread recv_thread(&udp_conn_worker::recv_run, worker);
			pin_thread(&recv_thread, recv_thost_id);
			recv_thread.detach();
		} else {
			tcp_conn_worker *worker = new tcp_conn_worker(work_lists[lid], worker_connect_speed);
			std::thread send_thread(&tcp_conn_worker::send_run, worker);
			pin_thread(&send_thread, send_thost_id);
			send_thread.detach();
			std::thread recv_thread(&tcp_conn_worker::recv_run, worker);
			pin_thread(&recv_thread, recv_thost_id);
			recv_thread.detach();
		}
	}
	delete[] work_lists;

	control.started = true;
	double ramp_start_time = clock_mono_nsec();
	printf("===ramp up started===\n");
	fflush(stdout);
	while (control.ramp_up_cnt != conn_cnt) {
		sleep(1);
	}
	printf("===ramp up finished (time: %fs)===\n", (clock_mono_nsec() - ramp_start_time) / 1.0e9);
	fflush(stdout);

	do_work();
	fflush(stdout);

	if (!conf.preload && conf.histogram_body > 0) {
		for (int i = 0; i < conn_cnt; i++) {
			conn_work *work = conn_works[i];
			work->dump_histogram("histograms");
		}
	}

	return 0;
}
