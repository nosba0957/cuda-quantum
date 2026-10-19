# Large Max-Cut graphs are split in the runtime, not by a compiler pass

A Max-Cut graph with more nodes than the QPU has qubits is split into subgraphs,
each solved by its own QAOA run, and their answers merged: the ADC-QAOA method.
This happens inside `cudaq::max_cut`, in the runtime library, not in an MLIR
pass. A pass sees only gates, after the graph has been lowered away; the graph
arrives with the program's arguments, not at compile time; and the merge is
host logic over measured bitstrings that no pass could run anyway.

The split and merge copy the Python ADC-QAOA (`experiments/ADC-QAOA`) exactly,
including its optimizer start and its choice of answers by count, so the two
can be compared on the same graph. Better splitting (LGP) is left for later.

## Consequences

Subgraphs of the same shape are renumbered into one order so they share a
circuit structure, and so one compile. That is the only reuse: tuned angles and
measured results are not shared between subgraphs.
