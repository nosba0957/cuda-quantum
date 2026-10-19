# 03: Live: a split `max_cut(9, seed)` closes its loop over OPNIC

**What to build:** One short run on the real OPX1000 over the **OPNIC transport**: a 9-node **Max-Cut graph**, split into 3 **subgraphs** (4, 4 and 3 nodes), with a small `max_eval`. Keep the program as short as possible, and run it only with the user's go-ahead (shared cluster).

**Blocked by:** 01 (`max_cut` splits large graphs on the simulator), 02 (Same-shape subgraphs share one compile)

**Status:** resolved

- [x] The run finishes and returns a bitstring for all 9 nodes
- [x] The QM id is printed at open and close, and nothing is left open
- [x] The expected compile count is recorded (the live run does not log compiles)
- [x] The cut is recorded, not judged: the chip's readout is skewed (qubit 0 reads 0, qubit 1 reads 1)

## Comments

**2026-10-19:** run live over OPNIC with the user's go-ahead:
`P5_SRC=tests/p5c_live.cpp run_p5_checks.sh --live --n 9 --maxeval 3 --qm-transport-opnic`.
It opened and closed `QM-1ae12d9d-e862-4b8e-a2a1-684a33070d68` and nothing
was left open. The whole run took 41 s and returned `001001001`, cut 13 (the
graph has 25 edges and a best cut of 17). The three subgraphs
(`0–3`, `3–6`, `6–8`) all have different shapes, so 3 compiles are expected.
The live run does not log compiles, so that count comes from the offline check
(one compile per shape, shown there for `max_cut(8, 3)` on 3 qubits), not from
this run: 3 is a prediction from the three shapes. The cut is not judged, because
the chip's readout is skewed.
