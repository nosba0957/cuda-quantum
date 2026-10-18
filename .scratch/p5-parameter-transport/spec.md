# P5 — Max-Cut in one call, compiled once, parameters over OPNIC

Status: ready-for-agent

Vocabulary: `GLOSSARY.md` (variational parameter, circuit structure, parameter
table, evaluation, parameter transport, Ethernet transport, OPNIC transport).
Design rationale: `docs/adr/0001-parameter-transport.md`.

## Problem Statement

A CUDA-Q user can run QAOA on the OPX1000 today (`tests/p5_qaoa.cpp` passes on
the mock), but only by writing the kernel, the cost function and the optimizer
loop themselves, and every evaluation recompiles the program because the angles
are baked into it. The user's own Python stack already does better on the same
instrument: its OPNIC path compiles once and runs 100 evaluations in 8.4 s,
against 160.4 s over Ethernet with a recompile per evaluation
(`ADC-QAOA/opnic_dgx_opx/COMPILE_TIME_ISSUE.md:266`).

Three things stand between the two:

1. CUDA-Q bakes every kernel argument into the circuit before code generation
   (`runtime/internal/compiler/Compiler.cpp:259`, `argument-synthesis`), so by
   the time the executor sees QASM, the variational parameters are anonymous
   numbers. The current `--qm-parametric` mode recovers them by blanking angles
   in the QASM text, one slot per gate, which pushes 10 numbers for a 2-parameter
   QAOA on a 5-ring and cannot tell `2*gamma` from an unrelated constant.
2. The executor has no way to talk to the OPNIC card.
3. There is no one-call entry point: a user wants to write `max_cut(5, seed)`.

## Solution

```cpp
#include <cudaq/algorithms/max_cut.h>
auto r = cudaq::max_cut(5, /*seed=*/3);   // r.bitstring, r.cut
```

`max_cut` draws the same random graph networkx would for that seed, builds the
QAOA circuit, runs COBYLA on the host, and returns the most frequent bitstring at
the best parameters and its cut. It runs on any target; on `quantum_machines` the
parameter transport is Ethernet by default and OPNIC with
`--qm-transport-opnic` (run time) or `--quantum_machines-transport-opnic`
(compile time; run time wins).

Under it, one compiler pass leaves the variational parameters open and records a
parameter table, so the executor recognises a repeated circuit structure and
sends only the parameters. With OPNIC that means one compile per circuit
structure and two numbers per evaluation for a p=1 QAOA.

Built in three phases, each verified offline before any instrument time:
**P5a** parameter table, **P5b** `max_cut`, **P5c** OPNIC transport.

## User Stories

1. As a CUDA-Q user, I want to solve Max-Cut with one call, so that I don't
   write a kernel, a cost function and an optimizer loop to try the OPX.
2. As a CUDA-Q user, I want `max_cut(n, seed)` to give me the graph networkx's
   `erdos_renyi_graph(n, 0.5, seed=seed)` gives, so that I can compare CUDA-Q
   runs with my Python ADC-QAOA runs graph for graph.
3. As a CUDA-Q user, I want the answer as a bitstring and its cut value, so
   that I can read off the partition directly.
4. As a CUDA-Q user, I want the defaults of my Python script (1 layer, 1024
   shots, 100 evaluations, initial step 0.5, density 0.5, initial parameters
   uniform in [0, 2π) from seed 42), so that the two stacks start equal.
5. As a CUDA-Q user, I want every default overridable, so that I can sweep
   layers or shots without editing the library.
6. As a CUDA-Q user, I want `max_cut` to run on the simulator, so that I can
   develop without instrument time.
7. As a CUDA-Q user, I want to pick OPNIC with one flag at compile or run time,
   so that one binary serves both links.
8. As a CUDA-Q user, I want a clear error when I ask for OPNIC on a machine or
   build without it, so that I don't silently fall back to Ethernet.
9. As a CUDA-Q user, I want my own kernels with a `std::vector<double>` argument
   to get the same compile-once treatment as `max_cut`, so that hand-written
   variational circuits are not second-class.
10. As a CUDA-Q user, I want a kernel whose parameters feed anything other than
    affine rotation angles to still run, with a warning that it recompiles each
    evaluation, so that correctness never depends on the optimisation.
11. As a physicist, I want OPNIC runs to compile once per circuit structure, so
    that a 100-evaluation QAOA costs seconds rather than minutes.
12. As a physicist, I want the OPX job halted when my run ends or fails, so that
    a program blocked waiting for parameters never holds the instrument.
13. As a target maintainer, I want every phase testable on the mock QOP and a
    fake OPNIC link, so that instrument time is spent only on the live gate.
14. As a target maintainer, I want the parameter table asserted from the
    receiving side, so that "two numbers per evaluation" is what the instrument
    saw rather than what the executor believed it sent.

## Implementation Decisions

### One pass, one setting (P5a)

The compiler change is shared by every transport. At JIT time, argument
synthesis bakes in every kernel argument **except** a `std::vector<double>`
argument, which is the variational parameters (the convention `cudaq::vqe`
already uses). After unrolling, a new pass traces each rotation angle back to
the open argument and requires the form `a * params[k] + b`. It emits:

- the circuit structure: the QASM with each such angle printed as a named
  expression of the parameters, e.g. `rz(2*p0)` — which *is* the parameter
  table, carried inline, so the two cannot drift apart; and
- the parameter values, in `KernelExecution::user_data["parameters"]`.

(As built, 2026-10-17: the pass tags each operator with
`cudaq.parameter_exprs` and the OpenQASM emitter prints the tag. It runs as
the `quantum_machines` target's `jit-low-level-pipeline`, after everything that
could fold its placeholder angles. `Compiler.cpp` leaves the argument open only
when that pipeline names the pass.)

If any use of the open argument is not of that form (a branch, a loop bound,
`sin(gamma)`, `gamma*beta`), the kernel is re-synthesised with the argument
baked in and a warning names the kernel: it recompiles every evaluation.

`qua_build.py` replaces the named expressions with qiskit `Parameter`s before
`qasm2.loads` (QASM2 cannot parse them), so the existing transpile and
`expr_to_qua` path is unchanged. The text-blanking `scanQasm` and its per-gate
slots are deleted once the table replaces them.

The executor's cache key becomes the circuit structure plus the settings it
already includes. With a table present and a compile-once transport, a hit sends
only the parameter values.

### Ethernet transport

The default. While the QOP does not advance input streams (memory:
`qm-input-streams-do-not-advance`), Ethernet substitutes the parameter values
into the structure and builds a static program per evaluation — today's
behaviour. The input-stream path stays behind `--qm-parametric` for testing and
becomes the Ethernet behaviour once the QOP honours it; the user-facing flag
does not change.

### OPNIC transport (P5c)

`QMExecutor` links QM's `opnic` C++ SDK (`/usr/include/opnic/opnic.hpp`,
`libopnic.so`) and calls it in-process; the user's separate `dgx_cpu_qaoa`
process is not reproduced. gRPC still opens the machine, compiles, queues and
halts; OPNIC carries parameters to the OPX and raw measurement bits back.

- QUA side, added by `qua_build.py`: `declare_input_stream("opnic", <in id>,
  <param struct>)` + `receive_from_stream`, and
  `declare_output_stream("opnic", <out id>, <result struct>)` + `send_to_stream`,
  modelled on `ADC-QAOA/opnic_dgx_opx/qaoa_with_opt.py:296-371`. Stream IDs are
  settings defaulting to that script's 987 (in) and 654 (out).
- Results are raw per-shot bits in packets that stay under the ~4 KB packet
  limit (oversized packets hang silently, `gpu/gpu_opnic.cu:14-20`), with a
  fixed packet count per evaluation (`qaoa_with_opt.py:364-366`).
- Streams are initialised and synced before the job is queued, as the Python
  path does.
- The program loops `--qm-iterations` evaluations and is requeued, without
  recompiling, when it runs out. The executor halts the job before closing the
  machine, on success and on failure.
- The OPNIC calls sit behind one narrow link interface with two
  implementations: `libopnic`, and an in-memory fake that plays the OPX for
  offline tests. CUDA-Q Realtime would be a third implementation if QM's
  bridge becomes available (ADR 0001).
- The `opnic` CMake package is optional. Without it the target builds, and
  selecting OPNIC throws an error saying the build has no OPNIC support. The
  devcontainer needs `/usr/include/opnic`, `/usr/lib/libopnic*` and
  `--device /dev/opnic0` mounted.

### `cudaq::max_cut` (P5b)

`runtime/cudaq/algorithms/max_cut.h`, next to `vqe.h`. User API layer, so
`snake_case` and the backward-compatibility rules of `CppAPICodingStyle.md`.

- `max_cut(std::size_t n, std::uint64_t seed, max_cut_options = {})` returns
  `{std::string bitstring; int cut;}`, `bitstring[i]` being vertex `i`'s side.
- The graph is `gnp_random_graph(n, p, seed)` reproduced exactly: Python's
  `random.Random(seed)` is MT19937 seeded by `init_by_array` over the seed's
  32-bit words, `random()` is `(a*2^26 + b) / 2^53` with `a = next>>5`,
  `b = next>>6`, and one draw per pair of `itertools.combinations(range(n), 2)`
  in order, edge kept when the draw `< p`.
- The circuit is the one in `tests/p5_qaoa.cpp` (H layer, `cx-rz(2γ)-cx` per
  edge, `rx(2β)` mixer), generalised to `layers`.
- The cost is the expected cut over the counts, maximised; the answer is the
  most frequent bitstring of a final sample at the best parameters (ties: the
  lexicographically smallest), and its cut.
- Options and defaults: `layers 1`, `shots 1024`, `max_eval 100`,
  `initial_step 0.5`, `tol 1e-5`, `density 0.5`, `init_seed 42` (initial
  parameters uniform in [0, 2π)).

CUDA-Q's `cobyla` is NLopt's, not scipy's, and two gaps must be closed for the
defaults to mean anything:

- NLopt bounds default to [-π, π] (`optimizers/nlopt/nlopt.cpp:47-50`), which
  excludes most of the [0, 2π) initial draw. `max_cut` passes unbounded limits,
  as scipy's COBYLA is unbounded.
- The wrapper ignores `step_size`, so `rhobeg` cannot be set. One
  `set_initial_step` call when `step_size` is given fixes it for every user.

Even so, NLopt's COBYLA and scipy's are different implementations; equal
defaults make runs comparable, not identical.

### Chip limits

A graph with more vertices than active qubits (5 on `as-qpu-5q4c`) is rejected
before submission, naming both numbers. Edges between non-adjacent qubits are
routed by the transpiler, as in P5's ring.

## Testing Decisions

Follow P4's rules: assert from the receiving side, and enter at the highest seam
the behaviour is visible from. Live runs report distributions and never assert
on them.

- **P5a**: `run_p5_checks.sh` extended. On the mock with `--qm-parametric`, a
  6-evaluation QAOA must show **one** `Compile` and, in the mock's RPC log, each
  push carrying exactly 2 values. A kernel using `sin(theta[0])` must still run,
  warn, and compile per evaluation. A FileCheck test in `cudaq/test/` pins the
  pass's output for the QAOA kernel: named expressions in the structure, the
  table in `user_data`, and the fallback.
- **P5b**: a gtest compares the graph generator with networkx edge lists for a
  set of `(n, seed)` pairs, generated once by the venv's networkx and checked
  in, with the networkx version recorded. A simulator run of `max_cut(5, seed)`
  must return a bitstring whose cut is the graph's true max cut on a graph where
  QAOA p=1 reliably finds it.
- **P5c**: the fake link plays the OPX: it receives parameter packets, records
  them, and returns configured bitstrings in correctly sized packets. Assert one
  `Compile` for the run, the parameters the fake received, and a `HaltJob` (or
  equivalent) in the mock log on both normal exit and a forced failure. The live
  gate, on reserved instrument time: `max_cut(5, seed)` over OPNIC completes
  100 evaluations with one compile and releases the machine.

## Risks to retire first

- **OPNIC streams cannot be initialised twice in one process** (the user's
  experience, 2026-10-17; the Python path starts a fresh process per subgraph
  for this reason). So the executor initialises each stream ID **once per
  process**, with packet types of fixed size (parameters padded to a maximum
  count, results in fixed-size packets), and every circuit structure's program
  declares the same IDs and types. **Retired 2026-10-18** by a live spike on
  `gh1` (ticket 01): two different programs, run back to back, were both
  served by streams initialised once, so OPNIC serves any number of circuit
  structures per process.
- ~~**Fixed-point range.**~~ Retired in P5a. After transpiling to the chip's
  basis, only `rz` carries a parameter, and QuAM's rz macro is a bare
  `frame_rotation`. `qua_build.py` now emits `frame_rotation_2pi` with the
  angle in turns, where an overflow loses only whole turns, and the manifest
  marks each parameter `periodic` when all its coefficients are whole, so the
  executor may fold the value it pushes into [-π, π). A parameter with a
  fractional coefficient and |p| ≥ 8 is a clear error.

## Out of Scope

- A QUA dialect in MLIR (`TODO.md`, P6).
- CUDA-Q Realtime as the OPNIC link (ADR 0001).
- ADC-QAOA (divide and conquer); it builds on `max_cut` later.
- How graphs other than Erdős–Rényi are passed in; deferred, `max_cut(n, seed)`
  only for now.
- Ethernet compile-once, until QM fixes input streams.
