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
#   P5_SRC=tests/p5c_live.cpp bash run_p5_checks.sh --live [args]
#       the P5c live programs instead (see that file; --n 9 runs a split
#       max_cut); live only
#
# args go to the binary first, so --maxeval/--layers/--qm-* override defaults.
# Offline it first checks qua_build.py's edge-mask guards (check_edge_mask.py),
# then runs three times: Ethernet (baked, a compile per evaluation), streamed
# (--qm-parametric: one compile; gamma, beta and the ring's edge mask pushed
# per evaluation),
# and OPNIC over an in-process fake of the card (one compile, nothing over
# gRPC but setup, and the waiting job cancelled at the end). Then, on the fake
# card, the P5c spike and tests/p5_split.cpp: four subgraphs on 3 or 2 qubits,
# two compiles; last, a silent card, whose wait for results must time out.
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
src="${P5_SRC:-tests/p5_qaoa.cpp}"

live=0
[ "${1:-}" = "--live" ] && { live=1; shift; }
[ "$live" = 1 ] || [ "$src" = tests/p5_qaoa.cpp ] || { echo "P5_SRC is live only"; exit 2; }

work="$(mktemp -d)"
cleanup() { [ -n "${mock_pid:-}" ] && kill "$mock_pid" 2>/dev/null; rm -rf "$work" 2>/dev/null; return 0; }
trap cleanup EXIT

fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

start_mock() {  # fresh mock and RPC log per mode, so counts are per mode
  [ -n "${mock_pid:-}" ] && kill "$mock_pid" 2>/dev/null
  rm -f "$work/ready" "$work/rpc.jsonl"
  # Half 01010 (cuts 4 ring edges), half 00000 (cuts none): every evaluation
  # must report an expected cut of exactly 2, whatever angles COBYLA picks.
  "$py" "$qmdir/tools/mock_qop.py" --listen "$endpoint" \
    --log "$work/rpc.jsonl" --ready-file "$work/ready" \
    --stream-frames 3 --stream-delay 0 --missing-polls 1 --results "$results" \
    > "$work/mock.log" 2>&1 &
  mock_pid=$!
  for _ in $(seq 60); do [ -f "$work/ready" ] && break; sleep 0.25; done
  if [ ! -f "$work/ready" ]; then cat "$work/mock.log"; exit 1; fi
}

# QM's opnic SDK lives on the host; the OPNIC plugin needs its library at run
# time, and the live card needs its device.
opnic_mounts=()
if [ -f /usr/lib/libopnic.so.1.0.0 ]; then
  opnic_mounts+=(-v /usr/lib/libopnic.so.1.0.0:/usr/lib/libopnic.so.1.0.0:ro)
  # The SDK sets streams up through the host's opnic-service socket.
  [ "$live" = 1 ] && [ -e /dev/opnic0 ] &&
    opnic_mounts+=(--device /dev/opnic0 -v /run/opnic:/run/opnic)
fi

# Runs the binary ($bin, default qaoa.out) with the given arguments in front
# of the defaults.
run_qaoa() {
  set +e
  local start; start=$(date +%s)
  docker run --rm --network host "${opnic_mounts[@]}" \
    -e CUDAQ_LOG_LEVEL="${CUDAQ_LOG_LEVEL:-}" \
    -v "$root":/workspace -v "$hostroot":"$hostroot" -v "$work":/work \
    -w /workspace/"${qmdir#$root/}" "$img" bash -lc '
  endpoint="$1"; cluster="$2"; py="$3"; state="$4"; shift 4
  [ -f /usr/lib/libopnic.so.1.0.0 ] && ln -sf libopnic.so.1.0.0 /usr/lib/libopnic.so.1
  command -v protoc > /dev/null || \
    { apt-get update -qq && apt-get install -y -qq libprotobuf-dev; } > /dev/null 2>&1
  mkdir -p /work/qmtmp
  /work/'"${bin:-qaoa.out}"' "$@" \
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
  evals=$(grep -c '^eval ' "$work/run.log" || true)
  check "binary exited ${want_rc:-0}"   "[ $rc -eq ${want_rc:-0} ]"
  if [ "${bin:-qaoa.out}" = qaoa.out ] && [ "$src" = tests/p5_qaoa.cpp ]; then
    check "COBYLA evaluated the cost"     "[ ${evals:-0} -ge 2 ]"
    check "the loop reported a best cut"  "grep -q '^best cut=' '$work/run.log'"
    echo "     $evals evaluations in ${elapsed}s"
  else
    echo "     ran in ${elapsed}s"
  fi
}

echo "### nvq++"
docker run --rm -v "$root":/workspace -v "$work":/work \
  -w /workspace/"${qmdir#$root/}" "$img" bash -lc '
  { apt-get update -qq && apt-get install -y -qq libprotobuf-dev; } > /dev/null 2>&1
  set -e
  /workspace/build/bin/nvq++ --target quantum_machines \
    --quantum_machines-url unused '"$src"' -o /work/qaoa.out
  [ '"$live"' = 1 ] || /workspace/build/bin/nvq++ --target quantum_machines \
    --quantum_machines-url unused tests/p5c_live.cpp -o /work/live.out
  [ '"$live"' = 1 ] || /workspace/build/bin/nvq++ --target quantum_machines \
    --quantum_machines-url unused tests/p5_split.cpp -o /work/split.out
  chown '"$(id -u):$(id -g)"' /work/*.out'

if [ "$live" = 1 ]; then
  endpoint="${QM_ENDPOINT:-10.21.19.201:9514}"
  cluster="${QM_CLUSTER:-QPX1000_4}"
  echo "### LIVE: $endpoint cluster $cluster -- this opens a quantum machine and plays pulses"
  # The gate is that the loop closes, not that it converges on this chip: the
  # cut values are reported, never asserted, live.
  run_qaoa "$@"
else
  endpoint="127.0.0.1:$port"
  cluster="mock"
  results=$("$py" -c "print(','.join(['01010','00000']*($shots//2)))")
  compiles() { grep -c "QmService/Compile" "$work/rpc.jsonl" || true; }

  echo "### qua_build.py: edge-mask guards equal plain QAOA for every mask"
  check "check_edge_mask.py" "'$py' '$here/check_edge_mask.py'"

  echo; echo "### Ethernet: parameters baked in, a compile per evaluation"
  start_mock
  run_qaoa --maxeval 6 "$@"
  check "every eval decoded to cut=2" "! grep '^eval ' '$work/run.log' | grep -qv 'cut=2.0000'"
  # The final sample at the best parameters may reuse one.
  c=$(compiles)
  check "a Compile per parameter set ($c)" "[ $c -ge $evals ] && [ $c -le $((evals + 1)) ]"
  check "nothing pushed" "! grep -q PushToInputStream '$work/rpc.jsonl'"
  check "results not yet registered are retried" "grep -q '\"missing\": true' '$work/rpc.jsonl'"

  # The edge-mask pass puts the ring's edges among all 10 pairs of 5 qubits,
  # (0,1) (0,2) ... (3,4), as parameters p2..p11 after gamma and beta.
  ring_mask="[1,0,0,1,1,0,0,1,0,1]"

  echo; echo "### Streamed: compiled once, gamma, beta and the edge mask pushed per evaluation"
  start_mock
  run_qaoa --maxeval 6 --qm-parametric 1 "$@"
  check "every eval decoded to cut=2" "! grep '^eval ' '$work/run.log' | grep -qv 'cut=2.0000'"
  c=$(compiles)
  check "one Compile for the whole optimization ($c)" "[ $c -eq 1 ]"
  pushes=$(grep -c PushToInputStream "$work/rpc.jsonl" || true)
  check "a push per sample ($pushes)" "[ $pushes -eq $((evals + 1)) ]"
  check "each push carries gamma, beta and the ring's edge mask" \
    "\"$py\" -c 'import json,sys; v=[json.loads(l) for l in open(sys.argv[1])]; p=[e for e in v if e[\"method\"].endswith(\"PushToInputStream\")]; sys.exit(0 if p and all(e[\"values\"][2:]==$ring_mask for e in p) else 1)' '$work/rpc.jsonl'"

  echo; echo "### OPNIC, fake link: compiled once, parameters and bits over OPNIC"
  start_mock
  run_qaoa --maxeval 6 --qm-transport-opnic --qm-opnic-fake "$results" \
    --qm-opnic-fake-log /work/opnic.jsonl "$@"
  check "every eval decoded to cut=2" "! grep '^eval ' '$work/run.log' | grep -qv 'cut=2.0000'"
  c=$(compiles)
  check "one Compile for the whole optimization ($c)" "[ $c -eq 1 ]"
  check "no parameters or results over gRPC" \
    "! grep -qE 'PushToInputStream|GetNamedResults' '$work/rpc.jsonl'"
  packets=$(wc -l < "$work/opnic.jsonl" 2>/dev/null || echo 0)
  check "an OPNIC parameter packet per sample ($packets)" "[ $packets -eq $((evals + 1)) ]"
  check "each packet is gamma, beta, the edge mask and 4 zeros of padding" \
    "\"$py\" -c 'import json,sys; p=[json.loads(l) for l in open(sys.argv[1])]; sys.exit(0 if p and all(len(v)==16 and v[2:12]==$ring_mask and not any(v[12:]) and any(v[:2]) for v in p) else 1)' '$work/opnic.jsonl'"
  check "the job left waiting for parameters is cancelled" \
    "grep -q 'JobService/Cancel' '$work/rpc.jsonl'"

  echo; echo "### OPNIC, fake link: two circuit structures on streams initialised once"
  start_mock
  bin=live.out run_qaoa --spike --qm-transport-opnic --qm-opnic-fake "$results" "$@"
  check "both structures returned their shots" "grep -q '^spike passed' '$work/run.log'"
  c=$(compiles)
  check "a Compile per structure ($c)" "[ $c -eq 2 ]"
  cancels=$(grep -c 'JobService/Cancel' "$work/rpc.jsonl" || true)
  check "the first job cancelled before the second, the second at close ($cancels)" \
    "[ $cancels -eq 2 ]"

  echo; echo "### OPNIC, fake link: a split max_cut compiles once per qubit count"
  start_mock
  bin=split.out run_qaoa --qm-transport-opnic --qm-opnic-fake "$results" "$@"
  check "the merged answer came back" "grep -q '^split max_cut ' '$work/run.log'"
  c=$(compiles)
  check "a Compile per qubit count, not per subgraph ($c)" "[ $c -eq 2 ]"

  echo; echo "### OPNIC, fake link: a card that never answers times out"
  start_mock
  want_rc=1 bin=live.out run_qaoa --spike --qm-transport-opnic \
    --qm-opnic-fake silent --qm-result-timeout 2 "$@"
  check "the wait for results timed out" \
    "grep -q 'timed out after 2s waiting for 1 OPNIC result packets' '$work/run.log'"
  check "the job left waiting is cancelled" \
    "grep -q 'JobService/Cancel' '$work/rpc.jsonl'"
fi

echo
[ "$fail" = 0 ] && echo "P5 PASS" || echo "P5 FAIL"
exit "$fail"
