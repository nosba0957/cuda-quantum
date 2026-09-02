#!/usr/bin/env bash
# ============================================================================ #
# Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
# Starts tools/mock_qop.py on the host, then builds and runs tests/p3_checks.cpp
# inside the CUDA-Q devcontainer against $CURL_INSTALL_PREFIX and the prebuilt
# build/lib -- so the checks drive the real QMExecutor, not a stand-in.
#
# The curl static libs must precede -lcudaq-common: that library exports the
# symbols of a libcurl statically linked into it, and if it is linked first the
# HTTP/2-less copy wins and every RPC fails.
#
# qua_build.py needs qiskit/quam from the host venv, which is not in the
# container, so the executor is pointed at tests/stub_qua_build.py. The
# subprocess contract is still exercised; what the stub cannot model -- which
# theta slots survive transpilation -- is covered offline against the real
# builder's checked-in corpus manifests.
#
#   bash run_p3_checks.sh                      mock on 127.0.0.1:9516
#   MOCK_PORT=9600 bash run_p3_checks.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qmdir="$(dirname "$here")"
root="$(cd "$qmdir/../../../../../../.." && pwd)"
img="${CUDAQ_DEVCONTAINER:-cuda-quantum-devcontainer:qm-http2}"
py="${PY:-/home/asrlabncku/as_ntu_ncku/.venv/bin/python}"
port="${MOCK_PORT:-9516}"
work="$(mktemp -d)"
cleanup() { [ -n "${mock_pid:-}" ] && kill "$mock_pid" 2>/dev/null; rm -rf "$work" 2>/dev/null; return 0; }
trap cleanup EXIT

"$py" "$qmdir/tools/mock_qop.py" --listen "127.0.0.1:$port" \
  --log "$work/rpc.jsonl" --ready-file "$work/ready" \
  --stream-frames 3 --stream-delay 0 --creg-name var6 \
  --results 00000,11111,10101,01010 > "$work/mock.log" 2>&1 &
mock_pid=$!
for _ in $(seq 60); do [ -f "$work/ready" ] && break; sleep 0.25; done
if [ ! -f "$work/ready" ]; then cat "$work/mock.log"; exit 1; fi
echo "mock QOP up on 127.0.0.1:$port (log $work/rpc.jsonl)"

docker run --rm --network host -v "$root":/repo -v "$work":/work \
  -w /repo/"${qmdir#$root/}" \
  -e STUB_QUA_PROGRAM=tests/corpus/qaoa_p1.golden.pb \
  "$img" bash -lc '
set -e
port="$1"; shift
prefix="${CURL_INSTALL_PREFIX:-/usr/local/curl}"
if ! "$prefix/bin/curl-config" --features 2>/dev/null | grep -qx HTTP2; then
  echo "### curl at $prefix has no HTTP/2, rebuilding it from the repo scripts"
  rm -f "$prefix/lib/libcurl.a"
  PREREQS_BUILD_DIR=$(mktemp -d) bash /repo/scripts/install_prerequisites.sh -m
fi
if ! command -v protoc > /dev/null; then
  apt-get update -qq && apt-get install -y -qq protobuf-compiler libprotobuf-dev
fi
b=$(mktemp -d)
protoc --cpp_out="$b" qm_min.proto
g++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter \
    -I"$b" -I. -I/repo/runtime -I/repo/runtime/include -I/repo/cudaq/include \
    -I/repo/tpls/json/single_include -I/repo/tpls/json/single_include_fwd \
    -I/repo/tpls/fmt/include -I/repo/tpls/spdlog/include -I"$prefix/include" \
    "$b"/qm_min.pb.cc GrpcCurl.cpp QuaResults.cpp QMExecutor.cpp \
    tests/p3_checks.cpp -o "$b/p3_checks" \
    -L"$prefix/lib" $("$prefix/bin/curl-config" --static-libs) \
    -L/repo/build/lib -lcudaq-common -lcudaq-logger -Wl,-rpath,/repo/build/lib \
    -lprotobuf-lite
mkdir -p /work/qmtmp
"$b/p3_checks" --corpus tests/corpus --rpc-log /work/rpc.jsonl \
    --qm-endpoint "127.0.0.1:$port" --qm-config tests/qua_config.pb \
    --qm-builder tests/stub_qua_build.py --qm-python python3 \
    --qm-tmpdir /work/qmtmp "$@"
rm -rf /work/qmtmp
' checks "$port" "$@"

echo
echo "--- RPC log ---"
cat "$work/rpc.jsonl"
