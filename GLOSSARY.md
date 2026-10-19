# CUDA-Q on the OPX1000

Running CUDA-Q kernels, and variational algorithms such as QAOA built from them,
on a Quantum Machines OPX1000.

## Language

**Variational parameter**:
A number the classical optimizer changes between evaluations, such as a QAOA
`gamma` or `beta`.
_Avoid_: angle (an angle is what a gate receives, possibly `2 * gamma`), theta

**Circuit structure**:
A circuit with its variational parameters left open. Two evaluations with the
same structure can share one compiled program.
_Avoid_: shape, template

**Parameter table**:
The record, kept beside a circuit structure, of which gate angles depend on which
variational parameter and how.
_Avoid_: angle slots, manifest

**Evaluation**:
One run of the circuit at one set of variational parameters, returning counts to
the optimizer.
_Avoid_: step, iteration (an iteration is one pass of the program's loop on the OPX)

**Parameter transport**:
How new variational parameters reach the OPX between evaluations: **Ethernet**
(the default) or **OPNIC**.
_Avoid_: streaming mode, parametric mode

**Ethernet transport**:
Parameters and results travel over the QOP's network API. Until the OPX honours
its input streams, every evaluation with new parameters is a fresh compile.
_Avoid_: gRPC mode, client stream

**OPNIC transport**:
Parameters and results travel over the OPNIC card's fiber link to the OPX; the
program is compiled once per circuit structure.
_Avoid_: fiber mode, NVQLink mode

**Max-Cut graph**:
The problem a Max-Cut run solves: an unweighted graph whose nodes are split
into two sides so that as many edges as possible cross between them.
_Avoid_: graph structure (that sounds like a circuit structure, which is a circuit,
not a problem), instance

**Subgraph**:
One part of a Max-Cut graph too large for the QPU, solved by its own QAOA run;
the answers of all subgraphs are merged into one cut of the whole graph.
_Avoid_: piece, chunk, SubQAOA

**Boundary node**:
The one node two neighbouring subgraphs share. The merge joins their answers
only where they put it on the same side.
_Avoid_: overlap node, connection point
