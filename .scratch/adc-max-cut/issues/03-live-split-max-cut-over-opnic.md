# 03: Live: a split `max_cut(9, seed)` closes its loop over OPNIC

**What to build:** One short run on the real OPX1000 over the **OPNIC transport**: a 9-node **Max-Cut graph**, split into 3 **subgraphs** (4, 4 and 3 nodes), with a small `max_eval`. Keep the program as short as possible, and run it only with the user's go-ahead (shared cluster).

**Blocked by:** 01 (`max_cut` splits large graphs on the simulator), 02 (Same-shape subgraphs share one compile)

**Status:** ready-for-agent

- [ ] The run finishes and returns a bitstring for all 9 nodes
- [ ] The QM id is printed at open and close, and nothing is left open
- [ ] The compile count is recorded
- [ ] The cut is recorded, not judged: the chip's readout is skewed (qubit 0 reads 0, qubit 1 reads 1)
