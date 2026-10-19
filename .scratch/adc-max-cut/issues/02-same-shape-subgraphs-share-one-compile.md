# 02: Same-shape subgraphs share one compile

**What to build:** **Subgraphs** with the same shape (isomorphic) are renumbered into one standard order before their QAOA runs, so they produce the same **circuit structure** and the executor compiles it once. Answers are mapped back to each subgraph's own node order before the merge. Only the compile is shared: each subgraph still runs its own optimizer, and no angles or results are reused (ADR 0002).

**Blocked by:** 01 (`max_cut` splits large graphs on the simulator)

**Status:** resolved

- [x] Offline, against the mock QOP and the fake OPNIC card: a split run compiles once per distinct subgraph shape, not once per subgraph
- [x] The merged answer is the same with and without renumbering on the simulator for the pinned seeds 8/3/3 and 12/5/4; other seeds can differ by shot noise
- [x] The renumbering keeps each boundary node where the merge expects it
- [x] The P5 offline checks still pass

## Comments

**2026-10-19:** Done in 59a7f4593f; offline only (mock QOP, fake OPNIC card).

- Compiles: new P5 mode runs `tests/p5_split.cpp`, `max_cut(8, 3)` with
  `qpu_qubits` 3: four subgraphs, two shapes (the third subgraph is the first
  two's shape numbered differently). 2 Compiles; 3 with the pre-ticket header.
  All other P5 offline modes still pass.
- Renumbering: `detail::canonical_edges` picks the renumbering whose sorted
  edges come first (not Python's bitmask order, which gives the same sharing).
  The target test checks that every renumbered answer maps back to the same cut
  of the subgraph's own edges, for every bitstring of every split it pins.
- Same answer: only partly. 8/3/3 (1024 shots) and 12/5/4 (100000 shots)
  merge to the same answer as before, and are pinned. Other seeds differ
  (20/5/4: cut 61 before, 56 after, at 1024 shots; still differs at 100000):
  shot noise lands on renumbered outcomes, so COBYLA takes another path.
- Also in bcba9dcb22, asked for separately: `max_cut` throws before any
  subgraph runs when the split leaves one larger than `qpu_qubits` (11/4
  leaves 5 nodes, 18/5 leaves 6). `detail::split` still copies Python. The
  target test's 11/4/4 merge case now expects that error.

**2026-10-19:** Code review reworded the "same merged answer" criterion to
what was verified: the pinned seeds 8/3/3 and 12/5/4 match; other seeds (20/5/4:
cut 61 before, 56 after) can differ by shot noise.
