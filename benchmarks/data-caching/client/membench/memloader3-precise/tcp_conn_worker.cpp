#include "tcp_conn_worker.h"

#include <sys/socket.h>
#include <netinet/tcp.h>
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
#include "util.h"
#include "config.h"
#include "memcached_cmd.h"
#include "tcp_request_sender.h"
#include "tcp_response_receiver.h"
#include "randnum.h"
#include "clock.h"

static int open_stream_sock(int work_id, const server_addr &saddr) {

	sockaddr sa;
	get_sockaddr(&sa, saddr.hostname, saddr.port, SOCK_STREAM);

	int sock = socket(AF_INET, SOCK_STREAM, 0);

	if (sock < 0) {
		perror("open_stream_sock: can't create sock");
		exit(1);
	}
 
	int flags = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));

	if (conf.base_port != 0) {
		bind_port(sock, conf.base_port + work_id, SOCK_STREAM);
	}

	if (connect(sock, &sa, sizeof(sa)) < 0) {
		perror("open_stream_sock: can't connect");
		exit(1);
	}
	if (!conf.nagles) {
		int optval = 1;
		if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(int)) < 0) {
			perror("open_stream_sock: can't turn off nagles");
			exit(1);
		}
	}
	return sock;
}

class tcp_request_queue {
private:
	std::mutex lock;
	std::list<request> queue;

public:
	bool empty() {
		lock.lock();
		bool res = queue.empty();
		lock.unlock();
		return res;
	}

	request &front() {
		lock.lock();
		request &res = queue.front();
		lock.unlock();
		return res;
	}

	void pop_front() {
		lock.lock();
		queue.pop_front();
		lock.unlock();
	}

	void push_back(request &r) {
		lock.lock();
		queue.push_back(r);
		lock.unlock();
	}
};

class tcp_recv_params {
public:
	conn_work *work;
	int sock;
	tcp_request_queue *outstandings;
public:
	tcp_recv_params() {}
	tcp_recv_params(conn_work *work, int sock, tcp_request_queue *outstandings)
	: work(work), sock(sock), outstandings(outstandings) {}
};

class tcp_send_context {
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
	tcp_request_sender sender;
	tcp_request_queue outstandings;

private:
	void connect() {
		sender.sock = open_stream_sock(work->id, work->saddr);
		work->client_port = get_socket_port(sender.sock);
		get_socket_ip(sender.sock, work->client_ip, IP_BUF_SZ);
		tcp_recv_params recv_params(work, sender.sock, &outstandings);
		int recv_param_sz = sizeof(recv_params);
		assert(write(signal_fd, &recv_params, recv_param_sz) == recv_param_sz);
	}

public:
	tcp_send_context(conn_work* work, int signal_fd, double first_target_start_point)
	: work(work), signal_fd(signal_fd) {
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
		sender.setup(pending_request);

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
		outstandings.push_back(pending_request);
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

static void tcp_recv_callback(evutil_socket_t sock, short what, void *arg);

class tcp_recv_context {
private:
	conn_work *work;
	tcp_response_receiver receiver;
	tcp_request_queue *outstandings;
	recv_state_t recv_state;
	event *readable;

public:
	tcp_recv_context(const tcp_recv_params &params, event_base *base) {
		work = params.work;
		receiver.sock = params.sock;
		outstandings = params.outstandings;
		recv_state = rst_running;
		readable = event_new(base, params.sock, EV_READ|EV_PERSIST, tcp_recv_callback, this); assert(readable != NULL);
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
		const auto& resp_vec = receiver.try_receive();
		if (resp_vec.empty()) {
			return false;
		}
		for (const auto& resp: resp_vec) {
			while (outstandings->empty()) {
				; // response arrives before request is enqueued
			}
			const request &r = outstandings->front();
			if (!request_response_match(r, resp)) {
				exit(1);
			}
			work->count_replied(r, resp);
			outstandings->pop_front();
		}
		return true;
	}
};

static void tcp_recv_callback(evutil_socket_t sock, short what, void *arg) {
	tcp_recv_context *ct = (tcp_recv_context*) arg;
	ct->continue_receive();
}

class tcp_send_context_comp {
public:
	bool operator() (const tcp_send_context *c0, const tcp_send_context *c1) {
		return c0->get_target_start_point() > c1->get_target_start_point();
	}
};

tcp_conn_worker::tcp_conn_worker(const std::list<conn_work*> &works, double worker_connect_speed)
: works(works), worker_connect_speed(worker_connect_speed) {
	assert(pipe2(signal_pipe, O_NONBLOCK) == 0);
}

void tcp_conn_worker::send_run() {

	while (!control.started);

	std::priority_queue<tcp_send_context*, std::vector<tcp_send_context*>, tcp_send_context_comp> queue;

	double connect_interval = 1.0e9 / worker_connect_speed;
	double first_target_start_point = 1.0e6 + clock_mono_nsec(); // 1ms

	rand_seed_t seed = (uint64_t) clock_mono_nsec() + pthread_self();
	rand_engine_t rg(seed);
	rand_uniform_real_t dist(1.0 - 0.5, 1.0 + 0.5);

	for (auto it = works.begin(); it != works.end(); it++) {
		queue.push(new tcp_send_context(*it, signal_pipe[1], first_target_start_point));
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

static void tcp_recv_new_conn_callback(evutil_socket_t fd, short what, void *arg) {
	assert(what == EV_READ);
	event_base *base = (event_base*) arg;
	tcp_recv_params params;
	assert(read(fd, &params, sizeof(params)) == sizeof(params));
	new tcp_recv_context(params, base);
}

void tcp_conn_worker::recv_run() {

	while (!control.started);

	event_config *cfg;
	event_base *base;
	int ret;

	cfg = event_config_new(); assert(cfg != NULL);
	ret = event_config_set_flag(cfg, EVENT_BASE_FLAG_NOLOCK); assert(ret == 0);
	base = event_base_new_with_config(cfg); assert(base != NULL);
	event_config_free(cfg);

	event *new_conn = event_new(base, signal_pipe[0], EV_READ|EV_PERSIST, tcp_recv_new_conn_callback, base); assert(new_conn != NULL);
	event_add(new_conn, NULL);

	if (conf.busy_loop_receive) {
		while (true) {
			event_base_loop(base, EVLOOP_NONBLOCK);
		}
	} else {
		event_base_dispatch(base);
	}
}
