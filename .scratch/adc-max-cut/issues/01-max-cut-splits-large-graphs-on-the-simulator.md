# 01: `max_cut` splits large graphs on the simulator

**What to build:** `cudaq::max_cut(n, seed)` on a **Max-Cut graph** with more nodes than `qpu_qubits` (a new option, default 5) returns one answer for the whole graph, the way the Python ADC-QAOA (`experiments/ADC-QAOA`, solver `adc_qaoa_ethernet`) does. See ADR 0002.

- Split: consecutive runs of node numbers, ⌈(n−1)/(q−1)⌉ **subgraphs**, neighbours sharing one **boundary node**, exactly as Python's `random_cut` with the driver's subgraph count.
- Each subgraph: its own QAOA, starting from Python's linear ramp (`linear_args(p, 0.56·p)`), COBYLA `tol 1e-5`, capped by `max_eval`. Keep its top *k* bitstrings by count (new option, default 1, as `--subqaoa_res_size`).
- Merge: all subgraphs in one go, adding inverted answers and joining only where the boundary node agrees, as Python's merge does.
- Subgraphs run one after another. Graphs that fit on the QPU take today's path unchanged.

**Blocked by:** None (can start immediately)

**Status:** resolved

- [x] For the same seed, the subgraphs match Python's split of `driver.py`'s graph (golden values produced by Python, as the existing target test does)
- [x] With every subgraph keeping all its bitstrings, the merged cut equals the brute-force best for graphs up to 12 nodes
- [x] `max_cut(5, 1)` gives the same result as before this ticket
- [x] The existing `max_cut` target test still passes

## Comments

**2026-10-19:** Done in 243b3f6a3b; `targettests/execution/max_cut.cpp` passes
on the simulator (qpp), no OPX contact.

- Split: `detail::split` matches Python's `random_cut` (nodes and edges of every
  subgraph) for n/q/seed = 12/5/4, 8/3/3, 11/4/4, 20/5/4. Python's last
  subgraph can exceed `qpu_qubits` (11/4/4 gives a 5-node one); copied as is.
- Merge: with every bitstring kept (100000 shots), 12/5/4, 8/3/3 and 11/4/4
  all reach the brute-force best cut. Breaking the boundary join fails it.
- `max_cut(5, 1)` (random seed 13) gives `01001`, cut 6, as before; now pinned.
- Differences from Python: COBYLA's step is still `initial_step` (0.5;
  scipy's default is 1.0) and `tol` is NLopt's relative f-tolerance, not
  scipy's rhoend. For p > 1 the start is the intended linear ramp; Python's
  circuit reads its `[betas, gammas]` vector interleaved (same at p = 1).
  Ties between equal counts or cuts are broken by bitstring / first found.
