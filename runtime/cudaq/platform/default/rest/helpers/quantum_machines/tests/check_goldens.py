#!/usr/bin/env python3
# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

"""Reload every corpus golden and check it against its manifest.

    check_goldens.py                  parse only, no server needed
    check_goldens.py --compile        also ask the live QOP to compile each one
"""

import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORPUS = HERE / "corpus"


def check_expr_to_qua():
    """expr_to_qua does plain arithmetic, so feeding it floats where QUA
    variables would go makes its result directly comparable to Qiskit's."""
    import math
    sys.path.insert(0, str(HERE.parent / "tools"))
    from qua_build import expr_to_qua
    from qiskit.circuit import Parameter

    g, b = Parameter("theta_0"), Parameter("theta_1")
    values = {g: 0.7, b: -1.3}
    for expr in (2 * g, g + b, -g + math.pi / 2, 3 * b - 2 * g + 5.0, g * 0 + 1.25):
        got = expr_to_qua(expr, {g: values[g], b: values[b]})
        want = float(expr.bind({p: values[p] for p in expr.parameters}))
        assert abs((got - want + math.pi) % (2 * math.pi) - math.pi) < 1e-12, \
            f"{expr}: {got} != {want} (mod 2pi)"

    try:
        expr_to_qua(g * b, {g: values[g], b: values[b]})
    except ValueError:
        pass
    else:
        raise AssertionError("a product of two parameters should be rejected")
    print("ok   expr_to_qua: affine angles reproduce qiskit, products rejected")


def check_parse():
    from qm.grpc.qm.pb import inc_qua_pb2

    names = sorted(p.stem.replace(".golden", "") for p in CORPUS.glob("*.golden.pb"))
    assert names, f"no goldens in {CORPUS}"

    for name in names:
        blob = (CORPUS / f"{name}.golden.pb").read_bytes()
        manifest = json.loads((CORPUS / f"{name}.manifest.json").read_text())

        prog = inc_qua_pb2.QuaProgram.FromString(blob)
        assert prog.SerializeToString() == blob, f"{name}: not a canonical round trip"
        assert prog.HasField("script"), f"{name}: no script"

        declared = {v.name for v in prog.script.variables if v.isInputStream}
        # An angle-free circuit declares no stream at all: there is nothing to
        # push, so the job runs its shots as soon as it starts.
        want = {manifest["input_stream"]} if manifest["input_stream"] else set()
        assert declared == want, \
            f"{name}: input streams {declared} != {want}"

        # resultAnalysis is a generic s-expression: saveAll(<creg>, buffer(<size>, ...)).
        for creg in manifest["result_streams"]:
            head = [v.string_value for v in prog.resultAnalysis.model[0].values[:2]]
            assert head == ["saveAll", creg["name"]], f"{name}: got {head}"
            buf = prog.resultAnalysis.model[0].values[2].list_value.values
            assert [buf[0].string_value, buf[1].string_value] == \
                ["buffer", str(creg["size"])], f"{name}: bad buffer shape"

        print(f"ok   {name}: {len(blob)} bytes, {len(manifest['angles'])} angles, "
              f"cregs={[(c['name'], c['size']) for c in manifest['result_streams']]}")
    return names


def check_compile(names):
    from qm import QuantumMachinesManager
    from qm.grpc.qm.pb import inc_qua_config_pb2, inc_qua_pb2
    from qm.program import Program

    config_pb = HERE / "qua_config.pb"
    assert config_pb.exists(), (
        f"{config_pb} missing; run tools/quam2pb.py -o {config_pb} first")
    config = inc_qua_config_pb2.QuaConfig.FromString(config_pb.read_bytes())

    caps = json.loads((HERE / "qua_config.pb.caps.json").read_text())
    host = os.environ.get("QOP_HOST", "10.21.19.201")
    port = int(os.environ.get("QOP_PORT", "9514"))
    cluster = os.environ.get("QOP_CLUSTER", "QPX1000_4")

    qmm = QuantumMachinesManager(host=host, port=port, cluster_name=cluster,
                                 timeout=300)
    print(f"QOP {qmm.version().QOP} at {host}:{port}, caps recorded from "
          f"{caps['qop_version']}")
    # Go through the v2 API with the pre-serialized message: qmm.open_qm()
    # would insist on a dict and re-run the conversion we are trying to test.
    # This is the same OpenQuantumMachine request the C++ executor will send.
    qm = qmm._api.open_qm(config, close_other_machines=False)
    try:
        for name in names:
            blob = (CORPUS / f"{name}.golden.pb").read_bytes()
            prog = Program(inc_qua_pb2.QuaProgram.FromString(blob))
            print(f"ok   {name}: program_id={qm.compile(prog)}")
    finally:
        qm.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compile", action="store_true",
                    help="also compile each golden on the live QOP")
    args = ap.parse_args()
    check_expr_to_qua()
    names = check_parse()
    if args.compile:
        check_compile(names)
    print("all good")
