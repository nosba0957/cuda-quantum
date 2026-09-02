#!/usr/bin/env python3
# ============================================================================ #
# Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
"""Stands in for tools/qua_build.py inside the devcontainer.

The real builder needs qiskit, quam and the host venv, none of which exist in
the container the HTTP/2 curl lives in. This honours the same subprocess
contract -- flags in, `--output` bytes and a `--manifest` JSON out -- deriving
the manifest from the QASM the way `parameterize` does, and dispensing the
corpus QuaProgram named by $STUB_QUA_PROGRAM so the mock still parses a genuine
program.

The manifest field this cannot fake is which theta slots survive transpilation;
p3_checks covers that against the real builder's checked-in manifests.
"""

import argparse
import json
import os
import re
import sys

ROT = {"rz", "rx", "ry", "p", "u1", "rzz", "crz", "crx", "cry", "cu1"}
GATE = re.compile(r"^\s*([A-Za-z_]\w*)\s*(\(([^)]*)\))?")
CREG = re.compile(r"^\s*creg\s+([A-Za-z_]\w*)\s*\[\s*(\d+)\s*\]\s*$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qasm", required=True)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--shots", type=int, default=1024)
    ap.add_argument("--iterations", type=int, default=1024)
    ap.add_argument("--optimization-level", type=int, default=1)
    ap.add_argument("--stream-name", default="angles")
    ap.add_argument("--reset-type", default="active")
    ap.add_argument("--reset-max-attempts", type=int, default=1)
    ap.add_argument("--caps-file")
    ap.add_argument("--state-path")
    args = ap.parse_args()

    text = re.sub(r"//[^\n]*", "", open(args.qasm).read())
    angles, cregs = [], []
    for stmt in text.split(";"):
        m = GATE.match(stmt)
        if not m:
            continue
        creg = CREG.match(stmt)
        if creg:
            cregs.append({"name": creg.group(1), "size": int(creg.group(2))})
            continue
        if m.group(3) is not None and m.group(1) in ROT:
            angles += [float(v) for v in m.group(3).split(",")]
    if not cregs:
        raise SystemExit("stub_qua_build: no creg in the QASM")

    program = os.environ["STUB_QUA_PROGRAM"]
    open(args.output, "wb").write(open(program, "rb").read())
    manifest = {
        "input_stream": f"input_stream_{args.stream_name}",
        "input_stream_type": "fixed",
        "input_stream_size": max(len(angles), 1),
        "angles": [f"theta_{i}" for i in range(len(angles))],
        "angle_values": angles,
        "shots": args.shots,
        "iterations": args.iterations,
        "result_streams": cregs,
        "qubits": [],
        "bytes": 0,
    }
    open(args.manifest, "w").write(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({
        "argv": sys.argv[1:],
        "manifest": manifest
    }),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
