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
the manifest from the `pK` parameters in the QASM, and dispensing the corpus
QuaProgram named by $STUB_QUA_PROGRAM so the mock still parses a genuine
program.

What this cannot fake is which parameters survive transpilation and whether
their coefficients are whole; it reports every parameter, periodic.
"""

import argparse
import json
import os
import re
import sys

PARAM = re.compile(r"\*p(\d+)")
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
    ap.add_argument("--bind")
    args = ap.parse_args()

    text = re.sub(r"//[^\n]*", "", open(args.qasm).read())
    cregs = []
    for stmt in text.split(";"):
        m = GATE.match(stmt)
        if not m:
            continue
        creg = CREG.match(stmt)
        if creg:
            cregs.append({"name": creg.group(1), "size": int(creg.group(2))})
            continue
    if not cregs:
        raise SystemExit("stub_qua_build: no creg in the QASM")

    program = os.environ["STUB_QUA_PROGRAM"]
    open(args.output, "wb").write(open(program, "rb").read())
    params = [] if args.bind else sorted({int(k) for k in PARAM.findall(text)})
    manifest = {
        "input_stream": f"input_stream_{args.stream_name}" if params else "",
        "input_stream_type": "fixed",
        "input_stream_size": len(params),
        "angles": [f"p{k}" for k in params],
        "periodic": [True] * len(params),
        "shots": args.shots,
        "iterations": args.iterations if params else 1,
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
