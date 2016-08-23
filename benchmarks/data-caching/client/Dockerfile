FROM ubuntu:latest
MAINTAINER Tapti Palit <tpalit@cs.stonybrook.edu>

# add our user and group first to make sure their IDs get assigned consistently, regardless of whatever dependencies get added
RUN groupadd -r memcache && useradd -r -g memcache memcache

ENV DEBIAN_FRONTEND noninteractive

RUN apt-get update && apt-get install -y libevent-2.0-5 --no-install-recommends \
	&& rm -rf /var/lib/apt/lists/*

COPY memloader.tar.gz memloader.tar.gz
COPY twitter.txt twitter.txt

RUN buildDeps='curl g++ libc6-dev libevent-dev make' \
	&& set -x \
	&& apt-get update && apt-get install -y $buildDeps --no-install-recommends \
	&& rm -rf /var/lib/apt/lists/* \
	#&& curl -SL "http://parsa.epfl.ch/cloudsuite/software/memcached.tar.gz" -o memcached.tar.gz \
	&& mkdir -p /usr/src/memloader \
	&& tar -xzf memloader.tar.gz -C /usr/src/memloader --strip-components=1 \
	&& rm memloader.tar.gz \
	&& cd /usr/src/memloader \
	&& make \
	&& chown -R memcache:memcache /usr/src/memloader \
	&& apt-get purge -y --auto-remove $buildDeps

COPY docker-entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
USER memcache
ENTRYPOINT ["/entrypoint.sh"]
CMD ["18000", "300"]
#ENTRYPOINT ["/bin/bash"]
