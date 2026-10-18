# 03: Live gate: `max_cut(5, seed)` over OPNIC

**What to build:** `cudaq::max_cut(5, seed)` with `--qm-transport-opnic` runs its whole optimization on the real OPX: parameters go out and measurement bits come back over the OPNIC card, and the program compiles once. `max_cut` uses one circuit structure, so it does not depend on the spike's answer.

**Blocked by:** None (can start immediately)

**Status:** ready-for-human

Runs only in the user's reserved OPX time, with their explicit go-ahead. Best done in the same booking as 01 and 04.

- [ ] 100 **evaluations**, exactly one `Compile`, no parameters or results over gRPC
- [ ] Every evaluation returns the requested shots; a top bitstring and its cut are returned
- [ ] The waiting job is cancelled at the end and the machine is released
- [ ] Wall time per evaluation is recorded (Python reference: 8.4 s per 100 evals); convergence is reported, not asserted
