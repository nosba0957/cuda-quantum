# 03: Live gate: `max_cut(5, seed)` over OPNIC

**What to build:** `cudaq::max_cut(5, seed)` with `--qm-transport-opnic` runs its whole optimization on the real OPX: parameters go out and measurement bits come back over the OPNIC card, and the program compiles once. `max_cut` uses one circuit structure, so it does not depend on the spike's answer.

**Blocked by:** None (can start immediately)

**Status:** resolved

Runs only in the user's reserved OPX time, with their explicit go-ahead. Best done in the same booking as 01 and 04.

- [ ] 100 **evaluations**, exactly one `Compile`, no parameters or results over gRPC
- [x] Every evaluation returns the requested shots; a top bitstring and its cut are returned
- [x] The waiting job is cancelled at the end and the machine is released
- [ ] Wall time per evaluation is recorded (Python reference: 8.4 s per 100 evals); convergence is reported, not asserted

## Comments

**2026-10-18, live on QPX1000_4 (commit 1407d0fb79):** `cudaq::max_cut(5, 1)`
over OPNIC, `--maxeval 10` (shortened from 100 at the user's request: keep
live programs short), returned `bitstring=10100 cut=4` in 53 s wall, including
nvq++, qua_build.py and Compile. `QM-aef2a424-…` opened and closed.

Not observed live: the Compile count and the per-evaluation time, because a
live run has no RPC log; one Compile and no parameters or results over gRPC
are asserted offline by `run_p5_checks.sh`. A 100-evaluation run with
`CUDAQ_LOG_LEVEL=info` would show both, if wanted. The chip's readout is off
(see ticket 01), so the answer is reported, not judged.
