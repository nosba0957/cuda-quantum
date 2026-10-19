# 02: Same-shape subgraphs share one compile

**What to build:** **Subgraphs** with the same shape (isomorphic) are renumbered into one standard order before their QAOA runs, so they produce the same **circuit structure** and the executor compiles it once. Answers are mapped back to each subgraph's own node order before the merge. Only the compile is shared: each subgraph still runs its own optimizer, and no angles or results are reused (ADR 0002).

**Blocked by:** 01 (`max_cut` splits large graphs on the simulator)

**Status:** ready-for-agent

- [ ] Offline, against the mock QOP and the fake OPNIC card: a split run compiles once per distinct subgraph shape, not once per subgraph
- [ ] The merged answer is the same with and without renumbering on the simulator for a fixed seed
- [ ] The renumbering keeps each boundary node where the merge expects it
- [ ] The P5 offline checks still pass
