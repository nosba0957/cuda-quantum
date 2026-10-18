# 04: Live QAOA loop over Ethernet

**What to build:** The P5 QAOA loop closes on the real chip (as-qpu-5q4c) over the default **Ethernet transport**: each evaluation with new parameters is a fresh compile, because QOP input streams do not advance.

**Blocked by:** None (can start immediately)

**Status:** resolved

Runs only in the user's reserved OPX time, with their explicit go-ahead. Best done in the same booking as 01 and 03.

- [x] The live mode of the P5 checks passes: every evaluation returns the requested shots and the run ends cleanly
- [ ] One `Compile` per distinct parameter set; the final sample at the best parameters is a cache hit
- [x] The machine is released afterwards; convergence is reported, not asserted

## Comments

**2026-10-18, live on QPX1000_4 (commit f687708b4d):** `run_p5_checks.sh
--live --maxeval 2` passed: 2 evaluations (cut 2.0000, then 3.9668), final
sample `{10010:1007, 00010:9, 10000:8}`, 49 s wall. `QM-25e6476d-…` opened
and closed. Compile count not observed live (no RPC log); asserted offline.

The run before it failed on evaluation 1 with only "nlopt failure": NLopt's
C++ wrapper swallows the objective's exception. `p5_qaoa.cpp` now prints the
cause first. The failure did not recur, so its cause is unknown; the next
occurrence will name it.
