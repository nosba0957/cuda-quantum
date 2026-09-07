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
b=/work
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
common="--corpus tests/corpus --qm-endpoint 127.0.0.1:$port
        --qm-config tests/qua_config.pb
        --qm-builder tests/stub_qua_build.py --qm-python python3
        --qm-parametric 1
        --qm-tmpdir /work/qmtmp"

# S6: without an explicit --mock, nothing that can queue a job may run.
echo "### S6: no --mock, must stay offline"
"$b/p3_checks" $common --rpc-log /work/rpc.jsonl > /work/s6.log 2>&1 \
  || { echo "FAIL S6: exited non-zero"; cat /work/s6.log; exit 1; }
if [ -s /work/rpc.jsonl ]; then
  echo "FAIL S6: RPCs were made without --mock"; cat /work/rpc.jsonl; exit 1
fi
grep -q "offline checks only" /work/s6.log \
  || { echo "FAIL S6: did not stop at the offline checks"; cat /work/s6.log; exit 1; }
echo "ok   S6: no --mock means no RPCs"

"$b/p3_checks" $common --rpc-log /work/rpc.jsonl --mock "$@"
rm -rf /work/qmtmp
chown -R '"$(id -u):$(id -g)"' /work
' checks "$port" "$@"

echo
echo "--- RPC log ---"
cat "$work/rpc.jsonl"

# S1: an assertion that fails while a quantum machine is open must still
# release it. The mock returns four identical shots, so "four distinct shots"
# fails naturally -- no test-only failure injection.
echo
echo "### S1: a failing assertion must still release the quantum machine"
kill "$mock_pid" 2>/dev/null || true
mock_pid=""
for _ in $(seq 40); do "$py" -c "
import socket,sys
s=socket.socket()
sys.exit(0 if s.connect_ex(('127.0.0.1',$port)) else 1)" && break; sleep 0.25; done

"$py" "$qmdir/tools/mock_qop.py" --listen "127.0.0.1:$port" \
  --log "$work/rpc-s1.jsonl" --ready-file "$work/ready-s1" \
  --stream-frames 3 --stream-delay 0 --creg-name var6 \
  --results 00000,00000,00000,00000 > "$work/mock-s1.log" 2>&1 &
mock_pid=$!
for _ in $(seq 60); do [ -f "$work/ready-s1" ] && break; sleep 0.25; done
if [ ! -f "$work/ready-s1" ]; then cat "$work/mock-s1.log"; exit 1; fi

set +e
docker run --rm --network host -v "$root":/repo -v "$work":/work \
  -w /repo/"${qmdir#$root/}" \
  -e STUB_QUA_PROGRAM=tests/corpus/qaoa_p1.golden.pb \
  "$img" bash -lc '
command -v protoc > /dev/null || \
  { apt-get update -qq && apt-get install -y -qq libprotobuf-dev; } > /dev/null 2>&1
mkdir -p /work/qmtmp
/work/p3_checks --corpus tests/corpus --qm-endpoint "127.0.0.1:'"$port"'" \
  --qm-config tests/qua_config.pb --qm-builder tests/stub_qua_build.py \
  --qm-python python3 --qm-tmpdir /work/qmtmp \
  --rpc-log /work/rpc-s1.jsonl --qm-parametric 1 --mock
rc=$?
chown -R '"$(id -u):$(id -g)"' /work
exit $rc
' > "$work/s1.log" 2>&1
rc=$?
set -e

fail=0
if [ "$rc" -eq 0 ]; then
  echo "FAIL S1: expected the run to fail, it passed"; fail=1
elif ! grep -q "four distinct shots" "$work/s1.log"; then
  echo "FAIL S1: failed for the wrong reason:"; tail -3 "$work/s1.log"; fail=1
elif ! grep -q "QmService/Close" "$work/rpc-s1.jsonl"; then
  echo "FAIL S1: the quantum machine was NOT released after the failure"
  echo "--- RPCs seen ---"; cut -c1-90 "$work/rpc-s1.jsonl"; fail=1
else
  echo "ok   S1: a failing assertion still releases the quantum machine"
fi
[ "$fail" = 0 ] || exit 1
