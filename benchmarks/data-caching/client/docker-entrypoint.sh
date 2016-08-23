#!/bin/bash
set -e
set -x

rps=$1
conns=$2

# Preload the server
/usr/src/memloader/memloader --server dc-server 11211 --db ./twitter.txt 5348680 --preload --load 100000

# Run the benchmark with the 5X scaled version

/usr/src/memloader/memloader --server dc-server 11211 --db ./twitter.txt 5348680 --set-ratio 0.2 --send-traffic-shape uniform 1.0 \
	--vclients $conns --load $rps --qos 1 --round 1 60 --round all 5000 2 --connect-speed 100 --connection-init-load 10 --connection-ramp-up-speed 300
