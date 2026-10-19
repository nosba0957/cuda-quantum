# 01: `max_cut` splits large graphs on the simulator

**What to build:** `cudaq::max_cut(n, seed)` on a **Max-Cut graph** with more nodes than `qpu_qubits` (a new option, default 5) returns one answer for the whole graph, the way the Python ADC-QAOA (`experiments/ADC-QAOA`, solver `adc_qaoa_ethernet`) does. See ADR 0002.

- Split: consecutive runs of node numbers, ⌈(n−1)/(q−1)⌉ **subgraphs**, neighbours sharing one **boundary node**, exactly as Python's `random_cut` with the driver's subgraph count.
- Each subgraph: its own QAOA, starting from Python's linear ramp (`linear_args(p, 0.56·p)`), COBYLA `tol 1e-5`, capped by `max_eval`. Keep its top *k* bitstrings by count (new option, default 1, as `--subqaoa_res_size`).
- Merge: all subgraphs in one go, adding inverted answers and joining only where the boundary node agrees, as Python's merge does.
- Subgraphs run one after another. Graphs that fit on the QPU take today's path unchanged.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

- [ ] For the same seed, the subgraphs match Python's split of `driver.py`'s graph (golden values produced by Python, as the existing target test does)
- [ ] With every subgraph keeping all its bitstrings, the merged cut equals the brute-force best for graphs up to 12 nodes
- [ ] `max_cut(5, 1)` gives the same result as before this ticket
- [ ] The existing `max_cut` target test still passes
