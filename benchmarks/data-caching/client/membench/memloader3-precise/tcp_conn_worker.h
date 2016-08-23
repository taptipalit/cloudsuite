#ifndef TCP_CONN_WORKER_H
#define TCP_CONN_WORKER_H

#include <list>
#include "conn_work.h"

class tcp_conn_worker {
private:
	const std::list<conn_work*> works;
	const double worker_connect_speed;
	int signal_pipe[2];

public:
	tcp_conn_worker(const std::list<conn_work*> &works, double worker_connect_speed);
	void send_run();
	void recv_run();
};

#endif
