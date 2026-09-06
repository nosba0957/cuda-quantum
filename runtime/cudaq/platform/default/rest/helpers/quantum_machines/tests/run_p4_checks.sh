#!/usr/bin/env bash
# ============================================================================ #
# Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
# P4: compile tests/p4_bell.cpp with nvq++ and assert on what cudaq::sample
# returns. The seam is the user binary -- everything below it (platform layer,
# executor, qua_build.py subprocess, transport, decoder) runs for real.
#
# Three mount facts are load-bearing:
#   * the repo must be at /workspace; nvq++ hard-codes that for dev-tree builds
#   * $hostroot must be at its own path; quam_libs is an editable install whose
#     finder holds absolute paths
#   * libprotobuf-lite is needed at run time, not just to compile
#
#   bash run_p4_checks.sh          mock QOP, no hardware touched
#   bash run_p4_checks.sh --live   REAL OPX1000: opens a machine, plays pulses
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qmdir="$(dirname "$here")"
root="$(cd "$qmdir/../../../../../../.." && pwd)"
img="${CUDAQ_DEVCONTAINER:-cuda-quantum-devcontainer:qm-http2}"
hostroot="${HOSTROOT:-/home/asrlabncku/as_ntu_ncku}"
py="${PY:-$hostroot/.venv/bin/python}"
state="${QUAM_STATE_PATH:-$hostroot/as-qpu-5q4c}"
port="${MOCK_PORT:-9517}"
shots=1024

live=0
[ "${1:-}" = "--live" ] && { live=1; shift; }

work="$(mktemp -d)"
cleanup() { [ -n "${mock_pid:-}" ] && kill "$mock_pid" 2>/dev/null; rm -rf "$work" 2>/dev/null; return 0; }
trap cleanup EXIT

if [ "$live" = 1 ]; then
  endpoint="${QM_ENDPOINT:-10.21.19.201:9514}"
  cluster="${QM_CLUSTER:-QPX1000_4}"
  echo "### LIVE: $endpoint cluster $cluster -- this opens a quantum machine and plays pulses"
else
  endpoint="127.0.0.1:$port"
  cluster="mock"
  # One mock result per shot, half 00 and half 11, so the decoded counts are
  # exact rather than plausible.
  results=$("$py" -c "print(','.join(['00','11']*($shots//2)))")
  "$py" "$qmdir/tools/mock_qop.py" --listen "$endpoint" \
    --log "$work/rpc.jsonl" --ready-file "$work/ready" \
    --stream-frames 3 --stream-delay 0 --creg-name var3 \
    --results "$results" > "$work/mock.log" 2>&1 &
  mock_pid=$!
  for _ in $(seq 60); do [ -f "$work/ready" ] && break; sleep 0.25; done
  if [ ! -f "$work/ready" ]; then cat "$work/mock.log"; exit 1; fi
  echo "### mock QOP up on $endpoint"
fi

set +e
docker run --rm --network host \
  -v "$root":/workspace -v "$hostroot":"$hostroot" -v "$work":/work \
  -w /workspace/"${qmdir#$root/}" "$img" bash -lc '
set -e
endpoint="$1"; cluster="$2"; py="$3"; state="$4"; shift 4
command -v protoc > /dev/null || \
  { apt-get update -qq && apt-get install -y -qq libprotobuf-dev; } > /dev/null 2>&1
b=$(mktemp -d)
echo "### nvq++"
/workspace/build/bin/nvq++ --target quantum_machines \
  --quantum_machines-url unused tests/p4_bell.cpp -o "$b/bell.out"
mkdir -p /work/qmtmp
echo "### run"
# Caller arguments go FIRST: lookupSetting takes the first match, so anything
# passed to this script must beat the defaults below (S5).
"$b/bell.out" "$@" \
  --qm-endpoint "$endpoint" --qm-cluster "$cluster" \
  --qm-config tests/qua_config.pb \
  --qm-builder tools/qua_build.py --qm-python "$py" \
  --qm-state "$state" --qm-tmpdir /work/qmtmp
rc=$?
chown -R '"$(id -u):$(id -g)"' /work
exit $rc
' checks "$endpoint" "$cluster" "$py" "$state" "$@" 2>&1 | tee "$work/run.log"
rc=${PIPESTATUS[0]}
set -e

echo
echo "--- asserting on what cudaq::sample returned ---"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

check "the binary exited 0"            "[ $rc -eq 0 ]"
check "counts include 00"              "grep -qE '\b00:' '$work/run.log'"
check "counts include 11"              "grep -qE '\b11:' '$work/run.log'"
if [ "$live" = 0 ]; then
  check "00 == $((shots/2))"           "grep -qE '\b00:$((shots/2))\b' '$work/run.log'"
  check "11 == $((shots/2))"           "grep -qE '\b11:$((shots/2))\b' '$work/run.log'"
  check "no 01"                        "! grep -qE '\b01:' '$work/run.log'"
  check "no 10"                        "! grep -qE '\b10:' '$work/run.log'"
  echo; echo "--- RPC log ---"; cat "$work/rpc.jsonl"
fi

echo
[ "$fail" = 0 ] && echo "P4 PASS" || echo "P4 FAIL"
exit "$fail"
