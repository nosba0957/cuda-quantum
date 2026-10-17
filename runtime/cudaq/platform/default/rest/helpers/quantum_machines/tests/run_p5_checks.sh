#!/usr/bin/env bash
# ============================================================================ #
# Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
# P5: compile tests/p5_qaoa.cpp with nvq++ and run its COBYLA loop. Same mount
# contract as run_p4_checks.sh, which documents it.
#
#   bash run_p5_checks.sh [args]          mock QOP, no hardware touched
#   bash run_p5_checks.sh --live [args]   REAL OPX1000: opens a machine, plays pulses
#
# args go to the binary first, so --maxeval/--layers/--qm-* override defaults.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qmdir="$(dirname "$here")"
root="$(cd "$qmdir/../../../../../../.." && pwd)"
img="${CUDAQ_DEVCONTAINER:-cuda-quantum-devcontainer:qm-http2}"
hostroot="${HOSTROOT:-/home/asrlabncku/as_ntu_ncku}"
py="${PY:-$hostroot/.venv/bin/python}"
state="${QUAM_STATE_PATH:-$hostroot/as-qpu-5q4c}"
port="${MOCK_PORT:-9518}"
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
  # Half 01010 (cuts 4 ring edges), half 00000 (cuts none): every evaluation
  # must report an expected cut of exactly 2, whatever angles COBYLA picks.
  results=$("$py" -c "print(','.join(['01010','00000']*($shots//2)))")
  "$py" "$qmdir/tools/mock_qop.py" --listen "$endpoint" \
    --log "$work/rpc.jsonl" --ready-file "$work/ready" \
    --stream-frames 3 --stream-delay 0 --results "$results" \
    > "$work/mock.log" 2>&1 &
  mock_pid=$!
  for _ in $(seq 60); do [ -f "$work/ready" ] && break; sleep 0.25; done
  if [ ! -f "$work/ready" ]; then cat "$work/mock.log"; exit 1; fi
  echo "### mock QOP up on $endpoint"
  set -- --maxeval 6 "$@"
fi

set +e
start=$(date +%s)
docker run --rm --network host \
  -e CUDAQ_LOG_LEVEL="${CUDAQ_LOG_LEVEL:-}" \
  -v "$root":/workspace -v "$hostroot":"$hostroot" -v "$work":/work \
  -w /workspace/"${qmdir#$root/}" "$img" bash -lc '
set -e
endpoint="$1"; cluster="$2"; py="$3"; state="$4"; shift 4
command -v protoc > /dev/null || \
  { apt-get update -qq && apt-get install -y -qq libprotobuf-dev; } > /dev/null 2>&1
b=$(mktemp -d)
echo "### nvq++"
/workspace/build/bin/nvq++ --target quantum_machines \
  --quantum_machines-url unused tests/p5_qaoa.cpp -o "$b/qaoa.out"
mkdir -p /work/qmtmp
echo "### run"
set +e
"$b/qaoa.out" "$@" \
  --qm-endpoint "$endpoint" --qm-cluster "$cluster" \
  --qm-config tests/qua_config.pb \
  --qm-builder tools/qua_build.py --qm-python "$py" \
  --qm-state "$state" --qm-tmpdir /work/qmtmp
rc=$?
chown -R '"$(id -u):$(id -g)"' /work
exit $rc
' checks "$endpoint" "$cluster" "$py" "$state" "$@" 2>&1 | tee "$work/run.log"
rc=${PIPESTATUS[0]}
elapsed=$(( $(date +%s) - start ))
set -e

echo
echo "--- asserting on the optimization loop ---"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

evals=$(grep -c '^eval ' "$work/run.log" || true)
check "binary exited 0"               "[ $rc -eq 0 ]"
check "COBYLA evaluated the cost"     "[ ${evals:-0} -ge 2 ]"
check "the loop reported a best cut"  "grep -q '^best cut=' '$work/run.log'"
echo "     $evals evaluations in ${elapsed}s (incl. nvq++)"

# The gate is that the loop closes, not that it converges on this chip: the
# cut values are reported, never asserted, live.
if [ "$live" = 0 ]; then
  compiles=$(grep -c "QmService/Compile" "$work/rpc.jsonl" || true)
  check "every eval decoded to cut=2"  "! grep '^eval ' '$work/run.log' | grep -qv 'cut=2.0000'"
  # Angles are baked in, so each new angle set is a new program. The final
  # sample at the best angles may reuse one.
  check "a Compile per angle set"      "[ $compiles -ge $evals ] && [ $compiles -le $((evals + 1)) ]"
  check "nothing pushed"               "! grep -q PushToInputStream '$work/rpc.jsonl'"
  echo "     $compiles Compile RPCs"
fi

echo
[ "$fail" = 0 ] && echo "P5 PASS" || echo "P5 FAIL"
exit "$fail"
