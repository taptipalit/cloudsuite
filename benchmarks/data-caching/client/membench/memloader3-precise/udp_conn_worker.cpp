#include "udp_conn_worker.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <assert.h>
#include <event2/event.h>
#include <string.h>
#include <queue>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <map>
#include <set>
#include "util.h"
#include "config.h"
#include "memcached_cmd.h"
#include "udp_request_sender.h"
#include "udp_response_receiver.h"
#include "randnum.h"
#include "clock.h"

static void open_udp_sock(int work_id, const server_addr &saddr, udp_request_sender *sender) {

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("open_udp_sock: can't create socket");
		exit(1);
	}

	int flags = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));

	if (conf.base_port != 0) {
		bind_port(sock, conf.base_port + work_id, SOCK_DGRAM);
	}

	sender->sock = sock;
	get_sockaddr(&sender->saddr, saddr.hostname, saddr.port, SOCK_DGRAM);
}

class udp_transaction {
public:
	enum state_t { empty, req_done, resp_in_progress };
	state_t state;
	int id;
	request req;
	response resp;
	int resp_segment_cnt;
	std::set<int> resp_missing_segments;
};

class udp_transaction_manager {
private:
	std::mutex lock;
	std::list<udp_transaction> time_queue;
	std::map<int, std::list<udp_transaction>::iterator> id_table;
	std::list<int> free_ids;
	conn_work *work;

private:
	int new_tc() {
		int id = free_ids.front();
		free_ids.pop_front();
		time_queue.emplace_back();
		auto it = time_queue.end(); it--;
		id_table[id] = it;
		it->id = id;
		it->state = udp_transaction::empty;
		return id;
	}

	void delete_tc(int id) {
		auto tq_it = id_table[id];
		time_queue.erase(tq_it);
		id_table.erase(id);
		// push_back to delay id reuse as much as possible
		free_ids.push_back(id);
	}

	bool try_create_transaction_helper(int *id) {
		// check expiration first, or expired transactions may consume too much memory
		double cur_time = clock_mono_nsec();
		while (!time_queue.empty()) {
			const auto& oldest_tc = time_queue.front();
			if (oldest_tc.state == udp_transaction::empty) {
				break;
			}
			double age = cur_time - oldest_tc.req.send_time;
			if (age < conf.udp_timeout * 1.0e6) {
				break;
			}
			work->count_udp_timeout();
			delete_tc(oldest_tc.id);
		}
		if (free_ids.empty()) {
			return false;
		}
		*id = new_tc();
		return true;
	}

	bool try_process_response_segment_helper(const response_segment &seg) {
		if (id_table.count(seg.udp_id) == 0) {
			fprintf(stderr, "UDP timeout too small: missing transaction\n");
			exit(1);
		}
		auto& tc = *id_table[seg.udp_id];
		if (tc.state == udp_transaction::empty) {
			return false;
		}
		if (tc.state == udp_transaction::req_done) {
			// first segment
			tc.resp_segment_cnt = seg.segment_cnt;
			for (int i = 0; i < seg.segment_cnt; i++) {
				tc.resp_missing_segments.insert(i);
			}
			tc.state = udp_transaction::resp_in_progress;
			if (seg.resp.recv_time <= tc.req.send_time) {
				fprintf(stderr, "UDP timeout too small: impossible latency (<= 0) occured\n");
				exit(1);
			}
		}
		// tc.state == udp_transaction::resp_in_progress;
		if (seg.cur_segment == 0) {
			tc.resp = seg.resp;
			if (!request_response_match(tc.req, tc.resp)) {
				fprintf(stderr, "UDP timeout too small: request response mismatch\n");
				exit(1);
			}
		} else {
			tc.resp.recv_time = seg.resp.recv_time;
		}
		if (tc.resp_missing_segments.count(seg.cur_segment) == 0) {
			fprintf(stderr, "UDP timeout too small: extra segments\n");
			exit(1);
		}
		tc.resp_missing_segments.erase(seg.cur_segment);
		if (tc.resp_missing_segments.empty()) {
			work->count_replied(tc.req, tc.resp);
			delete_tc(tc.id);
		}
		return true;
	}

public:
	udp_transaction_manager(conn_work *work): work(work) {
		// UDP-based memcached protocol uses two bytes to store request ID
		for (int i = 0; i <= 0xffff; i++) {
			free_ids.push_back(i);
		}
	}

	// Returns false when running out of ids, true otherwise.
	bool try_create_transaction(int *id) {
		lock.lock();
		bool res = try_create_transaction_helper(id);
		lock.unlock();
		return res;
	}

	void register_request(int id, const request &r) {
		lock.lock();
		auto& tc = *id_table[id];
		tc.req = r;
		tc.state = udp_transaction::req_done;
		lock.unlock();
	}

	// Returns false if request has not been registered (possible when reply
	// comes before sending thread registers request), true otherwise.
	bool try_process_response_segment(const response_segment &seg) {
		lock.lock();
		bool res = try_process_response_segment_helper(seg);
		lock.unlock();
		return res;
	}
};

class udp_recv_params {
public:
	conn_work *work;
	int sock;
	udp_transaction_manager *outstandings;
public:
	udp_recv_params() {}
	udp_recv_params(conn_work *work, int sock, udp_transaction_manager *outstandings)
	: work(work), sock(sock), outstandings(outstandings) {}
};

class udp_send_context {
private:
	conn_work *const work;
	const int signal_fd;
	double min_send_rate;
	double max_send_rate;
	double cur_send_rate;
	double ramp_up_speed;
	double send_interval;
	double target_start_point;
	double ramp_start_point;
	bool connected;
	udp_request_sender sender;
	udp_transaction_manager outstandings;

private:
	void connect() {
		open_udp_sock(work->id, work->saddr, &sender);
		work->client_port = get_socket_port(sender.sock);
		get_socket_ip(sender.sock, work->client_ip, IP_BUF_SZ);
		udp_recv_params recv_params(work, sender.sock, &outstandings);
		int recv_param_sz = sizeof(recv_params);
		assert(write(signal_fd, &recv_params, recv_param_sz) == recv_param_sz);
	}

public:
	udp_send_context(conn_work* work, int signal_fd, double first_target_start_point)
	: work(work), signal_fd(signal_fd), outstandings(work) {
		min_send_rate = work->init_send_rate;
		max_send_rate = work->send_rate;
		cur_send_rate = min_send_rate;
		ramp_up_speed = work->ramp_up_speed;
		send_interval = 1.0e9 / cur_send_rate;
		target_start_point = first_target_start_point;
		ramp_start_point = first_target_start_point;
		connected = false;
	}

	void send_next(rand_engine_t *rg) {

		if (!connected) {
			connect();
			connected = true;
			if (cur_send_rate == max_send_rate) {
				// no send rate ramp up, so signal now (the other ramp_up_cnt.fetch_add won't execute)
				control.ramp_up_cnt.fetch_add(1);
			}
		}

		request pending_request;
		work->make_request(&pending_request, rg);

		int udp_id = 0;
		while(!outstandings.try_create_transaction(&udp_id))
			;
		sender.setup(udp_id, pending_request);

		double start_point = clock_mono_nsec();
		while(target_start_point > start_point) {
			start_point = clock_mono_nsec();
		}
		while (!sender.try_send(&pending_request.send_time))
			;
		double finish_point = clock_mono_nsec();

		if (cur_send_rate < max_send_rate) {
			cur_send_rate = min_send_rate + ramp_up_speed * (finish_point - ramp_start_point) / 1.0e9;
			if (cur_send_rate >= max_send_rate) {
				cur_send_rate = max_send_rate;
				control.ramp_up_cnt.fetch_add(1);
			}
			send_interval = 1.0e9 / cur_send_rate;
		}

		work->count_send_timing(target_start_point, start_point, finish_point);
		work->count_sent(pending_request);
		outstandings.register_request(udp_id, pending_request);
	}

	void update_target_start_point(rand_engine_t *rg) {
		switch (conf.send_traffic_shape.shape) {
		case traffic_shape::UNIFORM:
			{
				double delta = conf.send_traffic_shape.param;
				rand_uniform_real_t dist(1.0 - delta, 1.0 + delta);
				target_start_point += send_interval * dist(*rg);
			}
			break;
		case traffic_shape::NORMAL:
			{
				double stddev = conf.send_traffic_shape.param;
				rand_normal_real_t dist(1.0, stddev);
				target_start_point += send_interval * dist(*rg);
			}
			break;
		case traffic_shape::PEAKS:
			{
				rand_uniform_int_t peaks_select(0,9);
				double mean;
				if (peaks_select(*rg) == 0) {
					mean = 9.0;
				} else {
					mean = 1.0 / 9.0;
				}
				double stddev = conf.send_traffic_shape.param;
				rand_normal_real_t dist(mean, stddev);
				target_start_point += send_interval * dist(*rg);
			}
			break;
		case traffic_shape::GAMMA:
			{
				double alpha = conf.send_traffic_shape.param;
				double beta = 1.0 / alpha;
				std::gamma_distribution<double> dist(alpha, beta);
				target_start_point += send_interval * dist(*rg);
			}
			break;
		case traffic_shape::EXPONENTIAL:
			{
				double lambda = conf.send_traffic_shape.param;
				std::exponential_distribution<double> dist(lambda);
				double mean = 1.0 / lambda;
				target_start_point += (send_interval / mean) * dist(*rg);
			}
			break;
		default:
			assert(false);
		}
	}

	double get_target_start_point() const {
		return target_start_point;
	}
};

enum recv_state_t {
	rst_running,
	rst_read_pause
};

static void udp_recv_callback(evutil_socket_t sock, short what, void *arg);

class udp_recv_context {
private:
	conn_work *work;
	udp_response_receiver receiver;
	udp_transaction_manager *outstandings;
	recv_state_t recv_state;
	event *readable;

public:
	udp_recv_context(const udp_recv_params &params, event_base *base) {
		work = params.work;
		receiver.sock = params.sock;
		outstandings = params.outstandings;
		recv_state = rst_running;
		readable = event_new(base, params.sock, EV_READ|EV_PERSIST, udp_recv_callback, this); assert(readable != NULL);
		event_add(readable, NULL);
	}

	void drive_state_machine() {
		switch(recv_state) {
		case rst_running:
		case rst_read_pause:
			if (!try_receive()) {
				recv_state = rst_read_pause;
				break;
			}
			recv_state = rst_running;
			break;
		default:
			assert(false);
		}
	}

	void continue_receive() {
		for (int i = 0; i < conf.receive_burst; i++) {
			drive_state_machine();
			if (recv_state != rst_running) {
				break;
			}
		}
	}

private:
	bool try_receive() {
		response_segment seg;
		if (!receiver.try_receive(&seg)) {
			return false;
		}
		while(!outstandings->try_process_response_segment(seg))
			;
		return true;
	}
};

static void udp_recv_callback(evutil_socket_t sock, short what, void *arg) {
	udp_recv_context *ct = (udp_recv_context*) arg;
	ct->continue_receive();
}

class udp_send_context_comp {
public:
	bool operator() (const udp_send_context *c0, const udp_send_context *c1) {
		return c0->get_target_start_point() > c1->get_target_start_point();
	}
};

udp_conn_worker::udp_conn_worker(const std::list<conn_work*> &works, double worker_connect_speed)
: works(works), worker_connect_speed(worker_connect_speed) {
	assert(pipe2(signal_pipe, O_NONBLOCK) == 0);
}

void udp_conn_worker::send_run() {

	while (!control.started);

	std::priority_queue<udp_send_context*, std::vector<udp_send_context*>, udp_send_context_comp> queue;

	double connect_interval = 1.0e9 / worker_connect_speed;
	double first_target_start_point = 1.0e6 + clock_mono_nsec(); // 1ms

	rand_seed_t seed = (uint64_t) clock_mono_nsec() + pthread_self();
	rand_engine_t rg(seed);
	rand_uniform_real_t dist(1.0 - 0.5, 1.0 + 0.5);

	for (auto it = works.begin(); it != works.end(); it++) {
		queue.push(new udp_send_context(*it, signal_pipe[1], first_target_start_point));
		first_target_start_point += connect_interval * dist(rg);
	}

	while (true) {
		auto cx = queue.top();
		cx->send_next(&rg);
		queue.pop();
		cx->update_target_start_point(&rg);
		queue.push(cx);
	}
}

static void udp_recv_new_conn_callback(evutil_socket_t fd, short what, void *arg) {
	assert(what == EV_READ);
	event_base *base = (event_base*) arg;
	udp_recv_params params;
	assert(read(fd, &params, sizeof(params)) == sizeof(params));
	new udp_recv_context(params, base);
}

void udp_conn_worker::recv_run() {

	while (!control.started);

	event_config *cfg;
	event_base *base;
	int ret;

	cfg = event_config_new(); assert(cfg != NULL);
	ret = event_config_set_flag(cfg, EVENT_BASE_FLAG_NOLOCK); assert(ret == 0);
	base = event_base_new_with_config(cfg); assert(base != NULL);
	event_config_free(cfg);

	event *new_conn = event_new(base, signal_pipe[0], EV_READ|EV_PERSIST, udp_recv_new_conn_callback, base); assert(new_conn != NULL);
	event_add(new_conn, NULL);

	if (conf.busy_loop_receive) {
		while (true) {
			event_base_loop(base, EVLOOP_NONBLOCK);
		}
	} else {
		event_base_dispatch(base);
	}
}
