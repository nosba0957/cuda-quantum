#!/usr/bin/env bash
# Builds and runs grpc_curl_spike.cpp inside the CUDA-Q devcontainer, which is
# where protoc and libprotobuf-lite are available. Uses --network host so the
# QOP at 10.21.19.201 is reachable.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qmdir="$(dirname "$here")"
img="${CUDAQ_DEVCONTAINER:-ghcr.io/nvidia/cuda-quantum-devcontainer:cu12.6-gcc12-main}"

docker run --rm --network host -v "$qmdir":/w -w /w "$img" bash -lc '
set -e
apt-get update -qq
apt-get install -y -qq protobuf-compiler libprotobuf-dev libcurl4-openssl-dev
mkdir -p /tmp/b
protoc --cpp_out=/tmp/b qm_min.proto
g++ -std=c++20 -O1 -I/tmp/b /tmp/b/qm_min.pb.cc tests/grpc_curl_spike.cpp \
    -o /tmp/b/spike -lprotobuf-lite -lcurl
/tmp/b/spike "$@"
' spike "$@"
