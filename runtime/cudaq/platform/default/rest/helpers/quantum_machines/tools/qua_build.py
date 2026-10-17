#!/usr/bin/env python3
# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

"""OpenQASM 2.0 on stdin -> serialized QuaProgram on stdout.

Angles printed as `a*pK+b` become runtime parameters fed by a QUA input
stream, unless --bind bakes values in.
"""

import argparse
import json
import math
import os
import re
import sys

# The parameter-table pass prints an angle that depends on the kernel's open
# variational parameters as `a*pK` or `a*pK+b`; qasm2 cannot parse that.
_EXPR = re.compile(r"^\s*(-?[0-9.eE+-]+)\*p(\d+)(?:\+(-?[0-9.eE+-]+))?\s*$")
_ARGS = re.compile(r"^(\s*[A-Za-z_]\w*\s*)\(([^()]*)\)")
# ponytail: sentinels assume no real angle is near 1e6 rad.
_SENTINEL = 1.0e6

# CUDA-Q emits `measure <qreg> -> <creg>[k];` when the qreg holds one qubit,
# which qiskit.qasm2 rejects as an unresolvable broadcast.
_MEAS = re.compile(r"^(\s*measure\s+)([A-Za-z_]\w*)(\s*->\s*[A-Za-z_]\w*\[\d+\]\s*;)")
_QREG = re.compile(r"^\s*qreg\s+([A-Za-z_]\w*)\s*\[\s*(\d+)\s*\]\s*;")


def normalize_qasm(text):
    sizes = {}
    for line in text.splitlines():
        m = _QREG.match(line)
        if m:
            sizes[m.group(1)] = int(m.group(2))
    out = []
    for line in text.splitlines():
        m = _MEAS.match(line)
        if m and sizes.get(m.group(2)) == 1:
            line = f"{m.group(1)}{m.group(2)}[0]{m.group(3)}"
        out.append(line)
    return "\n".join(out) + "\n"


def parametric_qasm(text):
    """Swap each printed parameter expression for a unique sentinel number.

    Returns the parseable text and {sentinel: (k, a, b)} so the expressions can
    be put back once qiskit has built the circuit.
    """
    exprs = {}
    out = []
    for line in text.splitlines():
        m = _ARGS.match(line)
        if m and "p" in m.group(2):
            args = []
            for tok in m.group(2).split(","):
                e = _EXPR.match(tok)
                if not e:
                    if "p" in tok:
                        raise SystemExit(f"unparseable parameter expression {tok!r}")
                    args.append(tok)
                    continue
                s = _SENTINEL + len(exprs)
                exprs[s] = (int(e.group(2)), float(e.group(1)), float(e.group(3) or 0))
                args.append(repr(s))
            line = f"{m.group(1)}({','.join(args)}){line[m.end():]}"
        out.append(line)
    return "\n".join(out) + "\n", exprs


def attach_parameters(qc, exprs):
    """Replace sentinel angles with `a * Parameter("pK") + b`."""
    from qiskit.circuit import Parameter

    params = {}
    out = qc.copy_empty_like()
    for inst in qc.data:
        op = inst.operation
        if any(isinstance(v, float) and v in exprs for v in op.params):
            new = []
            for v in op.params:
                if isinstance(v, float) and v in exprs:
                    k, a, b = exprs[v]
                    v = a * params.setdefault(k, Parameter(f"p{k}")) + b
                new.append(v)
            op = op.copy()
            op.params = new
        out.append(op, inst.qubits, inst.clbits)
    return out


def _index(p):
    return int(p.name[1:])


def _wrap(x, period=2 * math.pi):
    return (x + period / 2) % period - period / 2


def affine(expr):
    """Affine ParameterExpression -> (constant, {Parameter: coefficient})."""
    # Parameters are matched by equality, not identity: transpile() returns
    # expressions whose Parameter members compare equal to the originals but
    # are different objects.
    present = expr.parameters
    zero = {p: 0.0 for p in present}
    const = float(expr.bind(zero))

    coeffs = {}
    for qp in present:
        unit = {p: (1.0 if p == qp else 0.0) for p in present}
        c = float(expr.bind(unit)) - const
        if abs(c) > 1e-12:
            coeffs[qp] = c

    probe = {p: 0.37 + 0.11 * i for i, p in enumerate(sorted(present, key=str))}
    model = const + sum(c * probe[qp] for qp, c in coeffs.items())
    if abs(float(expr.bind(probe)) - model) > 1e-9:
        raise ValueError(f"angle expression is not affine in its parameters: {expr}")
    return const, coeffs


def expr_to_qua(expr, param_map, period=2 * math.pi):
    """Affine ParameterExpression -> QUA arithmetic over fixed variables.

    fixed is 4.28, so |value| must stay under 8; the expression is periodic in
    `period`, so folding the constant term keeps the runtime sum in range.
    """
    const, coeffs = affine(expr)
    out = _wrap(const, period)
    for qp, c in coeffs.items():
        out = out + c * param_map[qp]
    return out


def periodic(qc, params):
    """Per parameter: are all its coefficients whole numbers?

    Then shifting it by 2*pi shifts every angle by whole turns, so the C++ side
    may fold the value it pushes into [-pi, pi) to fit fixed.
    """
    whole = {p: True for p in params}
    for inst in qc.data:
        for v in inst.operation.params:
            if getattr(v, "parameters", None):
                for p, c in affine(v)[1].items():
                    if abs(c - round(c)) > 1e-9:
                        whole[p] = False
    return [whole[p] for p in params]


def emit_circuit(qc, machine, param_map):
    from qiskit.circuit import ParameterExpression
    from qm.qua import assign, declare

    idx = {q: qc.find_bit(q).index for q in qc.qubits}
    cregs = {c.name: declare(bool, value=[False] * c.size) for c in qc.cregs}

    for inst in qc.data:
        op, qubits = inst.operation, inst.qubits
        name = op.name

        if name == "barrier":
            qs = [machine.active_qubits[idx[q]] for q in qubits]
            qs[0].align(*qs[1:])
            continue

        if len(qubits) == 2:
            ctrl = machine.active_qubits[idx[qubits[0]]]
            tgt = machine.active_qubits[idx[qubits[1]]]
            (ctrl @ tgt).apply(name, *op.params)
            continue

        if len(qubits) != 1:
            raise ValueError(f"unsupported operand count for {name}: {len(qubits)}")

        qubit = machine.active_qubits[idx[qubits[0]]]
        # Only measure and reset cross from xy to the resonator; rz/sx are
        # xy-only and already ordered, and CZ aligns itself.
        crosses = name in ("measure", "reset")
        if crosses:
            qubit.align()

        params = list(op.params)
        if name == "rz" and getattr(params[0], "parameters", None):
            # What the rz macro does, but in turns: 2*gamma plus a transpiler
            # pi can pass 8 rad and overflow fixed, while an overflow in turns
            # drops only whole turns.
            qubit.xy.frame_rotation_2pi(
                expr_to_qua(params[0] / (2 * math.pi), param_map, period=1.0))
            continue
        if params and isinstance(params[0], ParameterExpression):
            params[0] = (expr_to_qua(params[0], param_map)
                         if params[0].parameters else _wrap(float(params[0])))

        result = qubit.apply(name, *params)
        if inst.clbits:
            for clbit in inst.clbits:
                creg, i = qc.find_bit(clbit).registers[0]
                assign(cregs[creg.name][i], result)

        if crosses:
            qubit.align()

    return cregs


def build(qc, machine, params, shots, iterations, stream_name):
    from qm.qua import (assign, declare, declare_input_stream, declare_stream,
                        fixed, for_, program, receive_from_stream, save,
                        stream_processing)

    with program() as prog:
        shot = declare(int)
        streams = {c.name: declare_stream() for c in qc.cregs}
        param_map = {p: declare(fixed) for p in params}

        machine.apply_all_flux_to_joint_idle()

        def shots_loop():
            with for_(shot, 0, shot < shots, shot + 1):
                cregs = emit_circuit(qc, machine, param_map)
                for c in qc.cregs:
                    for i in range(c.size):
                        save(cregs[c.name][i], streams[c.name])

        if params:
            iteration = declare(int)
            inp = declare_input_stream("client", stream_name, fixed,
                                       size=len(params))
            with for_(iteration, 0, iteration < iterations, iteration + 1):
                receive_from_stream(inp)
                for k, p in enumerate(params):
                    assign(param_map[p], inp[k])
                shots_loop()
        else:
            # No angles, so no stream to gate on: the job runs its shots once
            # and ends, and the C++ side requeues for the next batch.
            shots_loop()

        with stream_processing():
            for c in qc.cregs:
                streams[c.name].boolean_to_int().buffer(c.size).save_all(c.name)
    return prog


def strip_loc(msg):
    """Drop the `loc` debug strings qm stamps on every statement.

    qm._loc._get_loc() embeds this file's absolute path and source line into
    each QUA statement, which inflates the serialized size ~14x and makes it
    depend on where the tool was invoked from. The QOP compiles fine without
    them; they only decorate its error messages.
    """
    from google.protobuf.message import Message

    for field, value in list(msg.ListFields()):
        if field.name == "loc" and field.type == field.TYPE_STRING:
            msg.ClearField("loc")
        elif field.label == field.LABEL_REPEATED:
            if field.message_type is None:
                continue
            if field.message_type.GetOptions().map_entry:
                for v in value.values():
                    if isinstance(v, Message):
                        strip_loc(v)
            else:
                for v in value:
                    strip_loc(v)
        elif isinstance(value, Message):
            strip_loc(value)
    return msg


def set_capabilities(caps_file, machine):
    from qm.api.models.capabilities import QopCaps, offline_capabilities
    if caps_file:
        names = set(json.load(open(caps_file))["capabilities"])
        by_name = {c.qop_name: c for c in QopCaps.get_all()}
        missing = sorted(names - set(by_name))
        if missing:
            raise SystemExit(f"unknown capabilities {missing}")
        offline_capabilities.set([by_name[n] for n in names])
        return
    from qm import QuantumMachinesManager
    net = machine.network
    QuantumMachinesManager(host=net["host"],
                           port=net.get("port"),
                           cluster_name=net.get("cluster_name"))


def claim_stdout():
    """Hand back a private copy of fd 1 and point fd 1 at stderr.

    The qm SDK logs "Starting session: <uuid>" to stdout at import, and QuAM
    warns there too; either would be spliced into the protobuf stream.
    """
    real = os.dup(1)
    os.dup2(2, 1)
    return os.fdopen(real, "wb")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qasm", default=None, help="read QASM from here, not stdin")
    ap.add_argument("-o", "--output", default=None, help="write bytes here, not stdout")
    ap.add_argument("--shots", type=int, default=1024)
    ap.add_argument("--iterations", type=int, default=1024,
                    help="parameter pushes this job serves before it ends")
    ap.add_argument("--qubits", default=None,
                    help="comma-separated initial_layout into machine.active_qubits")
    ap.add_argument("--optimization-level", type=int, default=1)
    ap.add_argument("--stream-name", default="angles",
                    help="client input stream; its RPC name is 'input_stream_<this>'")
    ap.add_argument("--bind", default=None,
                    help="comma-separated values for p0, p1, ...: bake them in "
                    "instead of streaming them")
    ap.add_argument("--state-path", default=None)
    ap.add_argument("--caps-file", default=None,
                    help="a qua_config.pb.caps.json; without it capabilities are "
                    "fetched from the live QOP")
    ap.add_argument("--reset-type", choices=["active", "thermalize"], default="active")
    ap.add_argument("--reset-max-attempts", type=int, default=1)
    ap.add_argument("--manifest", default=None,
                    help="write the C++-side contract (stream, cregs, angles) here")
    ap.add_argument("--script", default=None, help="also write generate_qua_script here")
    ap.add_argument("--keep-loc", action="store_true",
                    help="keep qm's per-statement source-location strings")
    args = ap.parse_args(argv)

    out = None if args.output else claim_stdout()

    if args.state_path:
        os.environ["QUAM_STATE_PATH"] = args.state_path

    from qiskit import qasm2, transpile
    from quam_libs.components import QuAM
    from quam_libs.experiments.qiskit_circuit import create_target
    from quam_libs.experiments.qiskit_circuit.qiskit_to_qua import (
        ensure_resets_for_active_qubits, has_reset_at_boundary)

    machine = QuAM.load()
    set_capabilities(args.caps_file, machine)

    text = open(args.qasm).read() if args.qasm else sys.stdin.read()
    text, exprs = parametric_qasm(normalize_qasm(text))
    qc = attach_parameters(qasm2.loads(text), exprs)
    if not qc.cregs:
        raise SystemExit("circuit has no classical register, nothing to measure")

    layout = ([int(x) for x in args.qubits.split(",")]
              if args.qubits else list(range(qc.num_qubits)))
    if len(layout) != qc.num_qubits:
        raise SystemExit(f"--qubits has {len(layout)} entries but the circuit has "
                         f"{qc.num_qubits} qubits")

    for i in layout:
        reset = machine.active_qubits[i].macros["reset"]
        reset.reset_type = args.reset_type
        reset.max_attempts = args.reset_max_attempts
        if args.reset_type == "thermalize":
            reset.thermalize_time = machine.active_qubits[i].thermalization_time

    if args.bind is not None:
        values = [float(v) for v in args.bind.split(",") if v.strip()]
        missing = [p.name for p in qc.parameters if _index(p) >= len(values)]
        if missing:
            raise SystemExit(f"--bind has {len(values)} values; {missing} unbound")
        qc = qc.assign_parameters({p: values[_index(p)] for p in qc.parameters})

    qc = transpile(qc,
                   target=create_target(machine),
                   initial_layout=layout,
                   optimization_level=args.optimization_level)
    if not has_reset_at_boundary(qc):
        qc = ensure_resets_for_active_qubits(qc)

    params = sorted(qc.parameters, key=_index)
    prog = build(qc, machine, params, args.shots, args.iterations, args.stream_name)

    pb = prog.qua_program
    if not args.keep_loc:
        strip_loc(pb)
    blob = pb.SerializeToString()
    if args.output:
        open(args.output, "wb").write(blob)
    else:
        out.write(blob)
        out.flush()

    if args.script:
        from qm import generate_qua_script
        open(args.script, "w").write(generate_qua_script(prog))

    manifest = {
        "input_stream": f"input_stream_{args.stream_name}" if params else "",
        "input_stream_type": "fixed",
        "input_stream_size": len(params),
        "angles": [p.name for p in params],
        "periodic": periodic(qc, params),
        "shots": args.shots,
        "iterations": args.iterations if params else 1,
        "result_streams": [{"name": c.name, "size": c.size} for c in qc.cregs],
        "qubits": layout,
        "bytes": len(blob),
    }
    if args.manifest:
        open(args.manifest, "w").write(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
