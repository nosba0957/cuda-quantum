#!/usr/bin/env bash
# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
# Starts tools/mock_qop.py on the host, then builds and runs tests/p2_checks.cpp
# inside the CUDA-Q devcontainer, linked against $CURL_INSTALL_PREFIX -- the curl
# CUDA-Q itself links, not the distro one. If that curl has no HTTP/2, the repo's
# own install_prerequisites.sh is rerun in the container to rebuild it.
#
#   bash run_p2_checks.sh                     mock on 127.0.0.1:9515
#   MOCK_PORT=9600 bash run_p2_checks.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qmdir="$(dirname "$here")"
root="$(cd "$qmdir/../../../../../../.." && pwd)"
img="${CUDAQ_DEVCONTAINER:-cuda-quantum-devcontainer:qm-http2}"
py="${PY:-/home/asrlabncku/as_ntu_ncku/.venv/bin/python}"
port="${MOCK_PORT:-9515}"
delay="${STREAM_DELAY:-0.4}"
work="$(mktemp -d)"
trap 'rm -rf "$work"; [ -n "${mock_pid:-}" ] && kill "$mock_pid" 2>/dev/null' EXIT

"$py" "$qmdir/tools/mock_qop.py" --listen "127.0.0.1:$port" \
  --log "$work/rpc.jsonl" --ready-file "$work/ready" \
  --stream-frames 3 --stream-delay "$delay" --results 00,11,11,00 \
  > "$work/mock.log" 2>&1 &
mock_pid=$!
for _ in $(seq 60); do [ -f "$work/ready" ] && break; sleep 0.25; done
if [ ! -f "$work/ready" ]; then cat "$work/mock.log"; exit 1; fi
echo "mock QOP up on 127.0.0.1:$port (log $work/rpc.jsonl)"

docker run --rm --network host -v "$root":/repo -w /repo/"${qmdir#$root/}" \
  "$img" bash -lc '
set -e
prefix="${CURL_INSTALL_PREFIX:-/usr/local/curl}"
if ! "$prefix/bin/curl-config" --features 2>/dev/null | grep -qx HTTP2; then
  echo "### curl at $prefix has no HTTP/2, rebuilding it from the repo scripts"
  rm -f "$prefix/lib/libcurl.a"
  PREREQS_BUILD_DIR=$(mktemp -d) bash /repo/scripts/install_prerequisites.sh -m
fi
apt-get update -qq
apt-get install -y -qq protobuf-compiler libprotobuf-dev
b=$(mktemp -d)
protoc --cpp_out="$b" qm_min.proto
g++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter \
    -I"$b" -I. -I/repo/runtime -I"$prefix/include" \
    "$b"/qm_min.pb.cc GrpcCurl.cpp QuaResults.cpp tests/p2_checks.cpp \
    -o "$b/p2_checks" -lprotobuf-lite \
    -L"$prefix/lib" $("$prefix/bin/curl-config" --static-libs)
"$b/p2_checks" "$@"
' checks --mock --endpoint "127.0.0.1:$port" --stream-delay "$delay" \
  --config tests/qua_config.pb --program tests/corpus/bell.golden.pb "$@"

echo
echo "--- RPC log ---"
cat "$work/rpc.jsonl"
