# syntax=docker/dockerfile:1
# The sqlengine image: sqlnode (every cluster role) and the sqlengine CLI.
#
#   docker build -f deploy/Dockerfile -t sqlengine:dev .
#
# Behind a proxy that only tunnels HTTPS and re-signs TLS, hand the build its
# CA certificate and an HTTPS Ubuntu mirror:
#   docker build --secret id=ca,src=/path/to/ca.crt \
#     --build-arg UBUNTU_MIRROR=https://archive.ubuntu.com/ubuntu/ -f deploy/Dockerfile -t sqlengine:dev .

ARG UBUNTU_MIRROR=

FROM ubuntu:24.04 AS base
ARG UBUNTU_MIRROR
# apt-install PACKAGES...: with the optional mirror and proxy CA
RUN printf '%s\n' '#!/bin/sh' 'set -e' \
      'if [ -n "$UBUNTU_MIRROR" ]; then sed -i -E "s#http://(archive|security).ubuntu.com/ubuntu/?#$UBUNTU_MIRROR#" /etc/apt/sources.list.d/ubuntu.sources; fi' \
      'ca=""; if [ -s /run/secrets/ca ]; then ca="-o Acquire::https::CAInfo=/run/secrets/ca"; fi' \
      'apt-get $ca update' \
      'DEBIAN_FRONTEND=noninteractive apt-get $ca install -y --no-install-recommends "$@"' \
      'rm -rf /var/lib/apt/lists/*' > /usr/local/bin/apt-install \
 && chmod +x /usr/local/bin/apt-install

FROM base AS build
RUN --mount=type=secret,id=ca,required=false,mode=0444 apt-install g++ cmake make git ca-certificates libssl-dev
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake cmake
COPY src src
# NuRaft and asio are fetched at configure time (cmake/NuRaft.cmake)
RUN --mount=type=secret,id=ca,required=false,mode=0444 \
    if [ -s /run/secrets/ca ]; then \
      cp /run/secrets/ca /usr/local/share/ca-certificates/build-proxy.crt && update-ca-certificates; \
    fi \
 && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
 && cmake --build build --parallel --target sqlnode sqlengine \
 && strip build/src/server/sqlnode build/src/sqlengine

FROM base
RUN --mount=type=secret,id=ca,required=false,mode=0444 apt-install libssl3t64 \
 && useradd --system --uid 10001 --home-dir /data sql \
 && mkdir -p /data && chown sql /data
COPY --from=build /src/build/src/server/sqlnode /src/build/src/sqlengine /usr/local/bin/
COPY deploy/entrypoint.sh deploy/workload.sh /usr/local/bin/
USER sql
WORKDIR /data
VOLUME /data
# clients, coordinator Raft, storage node calls; storage replicas use 20000 + shard
EXPOSE 7480 7100 7500
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
