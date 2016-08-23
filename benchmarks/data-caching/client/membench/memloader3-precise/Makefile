
PREFIX_DIR = /home/compassys/memcached_home/apps/memloader3-precise
CXX = g++
CXXFLAGS = -g -O3 -Wall -std=gnu++11 -pthread

.PHONY : all install clean

all : memloader
memloader : memloader.o conn_work.o memcached_cmd.o memdb.o util.o tcp_conn_worker.o tcp_request_sender.o tcp_response_receiver.o udp_conn_worker.o udp_request_sender.o udp_response_receiver.o thread_utils.o clock.o
	$(CXX) $(CXXFLAGS) $^ -o $@ -lm -levent

%.o : %.cpp *.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

install : all
	cp memloader $(PREFIX_DIR)/bin/memloader

clean :
	rm -f memloader *.o
