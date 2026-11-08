# ============================================================================ #
# Copyright (c) 2026 NVIDIA Corporation & Affiliates.                          #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
"""Offline check of qua_build.py's handling of the edge-mask pass's guards.

For every mask of a 4-qubit QAOA layer, the circuit built from the guarded
QASM (`if(pK==1)` per gate), transpiled onto a line with routing inside the
guards and its guards then resolved by the mask, must equal the plain QAOA of
that graph. --bind must give the same circuit with no guards left.

    python check_edge_mask.py      (needs only qiskit)
"""

import itertools
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import qua_build as Q  # noqa: E402

from qiskit import QuantumCircuit, qasm2, transpile  # noqa: E402
from qiskit.quantum_info import Operator  # noqa: E402

N = 4
PAIRS = list(itertools.combinations(range(N), 2))
GAMMA, BETA = 0.37, 0.81


def guarded_qasm():
    lines = ['OPENQASM 2.0;', 'include "qelib1.inc";', f"qreg q[{N}];"]
    lines += [f"h q[{i}];" for i in range(N)]
    for k, (i, j) in enumerate(PAIRS, start=2):
        g = f"if(p{k}==1) "
        lines += [f"{g}cx q[{i}],q[{j}];", f"{g}rz(2*p0) q[{j}];",
                  f"{g}cx q[{i}],q[{j}];"]
    lines += [f"rx(2*p1) q[{i}];" for i in range(N)]
    return "\n".join(lines) + "\n"


def plain(mask):
    qc = QuantumCircuit(N)
    qc.h(range(N))
    for on, (i, j) in zip(mask, PAIRS):
        if on:
            qc.rzz(2 * GAMMA, i, j)
    qc.rx(2 * BETA, range(N))
    return qc


def resolve(qc, values):
    """Inline each if_test whose input is 1, drop the others; bind angles."""
    out = QuantumCircuit(*qc.qregs)
    for inst in qc.data:
        op = inst.operation
        if op.name == "if_else":
            if values[Q._index(op.condition)] > 0.5:
                body = op.blocks[0]
                for b in body.data:
                    out.append(b.operation,
                               [inst.qubits[body.find_bit(q).index] for q in b.qubits])
        else:
            out.append(op, inst.qubits)
    return bind(out, values)


def bind(qc, values):
    return qc.assign_parameters({p: values[Q._index(p)] for p in qc.parameters})


def main():
    text, exprs = Q.parametric_qasm(Q.normalize_qasm(guarded_qasm()))
    qc = Q.attach_parameters(qasm2.loads(Q.masked_qasm(text)), exprs)
    names = [p.name for p in Q.runtime_values(qc)]
    assert names == [f"p{k}" for k in range(2 + len(PAIRS))], names
    assert sum(i.operation.name == "if_else" for i in qc.data) == len(PAIRS)

    line = [[a, b] for a in range(N - 1) for b in (a, a + 1) if a != b]
    line += [[b, a] for a, b in line]
    tq = transpile(qc, basis_gates=["rz", "sx", "x", "cz", "if_else"],
                   coupling_map=line, initial_layout=list(range(N)),
                   optimization_level=1, seed_transpiler=1)

    for mask in itertools.product([0, 1], repeat=len(PAIRS)):
        values = [GAMMA, BETA, *mask]
        want = Operator(plain(mask))
        baked = Q.attach_parameters(qasm2.loads(Q.masked_qasm(text)), exprs,
                                    values)
        for got in (resolve(qc, values), resolve(tq, values), bind(baked, values)):
            assert want.equiv(Operator(got)), f"mismatch for mask {mask}"
    print(f"edge-mask: all {2 ** len(PAIRS)} masks match plain QAOA")


if __name__ == "__main__":
    main()
