# 04: Live QAOA loop over Ethernet

**What to build:** The P5 QAOA loop closes on the real chip (as-qpu-5q4c) over the default **Ethernet transport**: each evaluation with new parameters is a fresh compile, because QOP input streams do not advance.

**Blocked by:** None (can start immediately)

**Status:** ready-for-human

Runs only in the user's reserved OPX time, with their explicit go-ahead. Best done in the same booking as 01 and 03.

- [ ] The live mode of the P5 checks passes: every evaluation returns the requested shots and the run ends cleanly
- [ ] One `Compile` per distinct parameter set; the final sample at the best parameters is a cache hit
- [ ] The machine is released afterwards; convergence is reported, not asserted
