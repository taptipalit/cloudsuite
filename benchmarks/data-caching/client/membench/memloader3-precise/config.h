#ifndef CONFIG_H
#define CONFIG_H

#include <list>
#include <vector>
#include <sys/socket.h>
#include <atomic>
#include "memdb.h"
#include "memcached_cmd.h"

class server_addr {
public:
	const char *hostname;
	const char *port;
};

class server_record {
public:
	std::list<server_addr> addrs;
	const memdb *db;
};

class work_round {
public:
	bool discrete;
	bool accumulate;
	int iter_cnt;
	int interval;
};

class traffic_shape {
public:
	enum shape_t {UNIFORM, NORMAL, PEAKS, GAMMA, EXPONENTIAL};
	shape_t shape;
	double param;
};

class config {
public:
	/* db stuff */
	const char *db_sample_file;
	int db_size;
	/**/

	/* server stuff */
	std::vector<server_record> servers;
	bool mirror;
	/**/

	/* vclient stuff */
	// For sharded setup, each vclient connects to all servers; 
	// for mirrored setup, each vclient connects to just one server.
	int vclients;
	/**/

	/* benchmark stuff */
	double load;
	double qos; // in ms
	double udp_timeout; // in ms, only for udp
	std::vector<work_round> work_rounds;
	/**/

	/* per connection stuff */
	bool udp;
	bool nagles;
	memcmd_t default_cmd;
	bool enumerate_items;
	bool set_miss;
	/**/

	bool preload;

	int base_port;

	double set_ratio;

	int per_connection_work;

	int histogram_head;
	int histogram_body;

	traffic_shape send_traffic_shape;

	bool busy_loop_receive;
	int receive_burst;

	double connect_speed; // new connection per second
	double connection_init_load;
	double connection_ramp_up_speed; // per connection load increament per second

	int mtu;
};

extern config conf;

class controller {
public:
	std::atomic_bool started;
	std::atomic_int ramp_up_cnt;
public:
	controller() : started(false), ramp_up_cnt(0) {}
};

extern controller control;

#endif
