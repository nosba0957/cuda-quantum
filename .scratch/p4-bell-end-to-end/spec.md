# P4 — Bell pair end to end on the OPX1000

Status: ready-for-agent

## Problem Statement

A user who has installed this fork cannot run a CUDA-Q kernel on the OPX1000 at
all. Every `cudaq::sample` against `--target quantum_machines` aborts before a
single byte reaches the instrument:

```
what():  quantum_machines: cudaq::run is not supported by this target
```

Three further problems sit behind it:

1. The Python builder cannot load the QuAM state the project was pointed at.
   The `as-qpu-10q9c` state references a pulse class that the installed
   `quam_libs` does not define, so the builder fails identically on the host and
   in the container. The corpus goldens were produced from an earlier revision
   of that state and can no longer be reproduced.
2. The test suite can fire pulses on real hardware by accident. One check
   program has no mock opt-in, and both driver scripts append user arguments
   after their own, where first-match-wins silently discards them.
3. A failing assertion in the live checks terminates the process in a way that
   skips destructors, leaving a quantum machine open on the QOP. This has
   happened three times.

## Solution

`nvq++ --target quantum_machines bell.cpp` produces a binary that, when run,
returns real measurement counts from the OPX1000 through `cudaq::sample`.

The user writes an ordinary CUDA-Q kernel. The target compiles it to OpenQASM 2
at run time, hands it to the Python builder once per circuit structure, QOP
compiles the resulting QUA program on the instrument, queues a job, and decodes
the returned buffers back into a `sample_result`. Nothing about the user's
kernel or their invocation is OPX-specific beyond the target name and a handful
of connection arguments.

Alongside it, the target is retargeted onto a QuAM state that actually loads,
the three safety findings are closed, and one end-to-end test seam is added that
enters through the public `cudaq::sample` API rather than through the executor.

## User Stories

1. As a CUDA-Q user, I want `cudaq::sample` to return counts from my OPX1000, so
   that I can run a quantum kernel on real hardware instead of a simulator.
2. As a CUDA-Q user, I want to select the OPX by writing
   `--target quantum_machines`, so that I do not have to learn a second toolchain.
3. As a CUDA-Q user, I want my kernel source to stay unmodified between the
   simulator and the OPX, so that I can develop offline and deploy without a
   rewrite.
4. As a CUDA-Q user, I want the counts I get back keyed by the classical
   register name I declared, so that the result reads the same as it does on any
   other backend.
5. As a CUDA-Q user, I want the number of returned shots to equal the number I
   asked for, so that I can normalise probabilities without guessing.
6. As a CUDA-Q user, I want a two-qubit Bell kernel to produce predominantly
   `00` and `11`, so that I have evidence the entangling pulse actually ran.
7. As a CUDA-Q user, I want to pass the connection details at run time, so that
   I can point one compiled binary at a different cluster without recompiling.
8. As a CUDA-Q user, I want to pass the same connection details at compile time
   instead, so that I can ship a binary that needs no arguments.
9. As a CUDA-Q user, I want the environment to supply those details as a third
   option, so that CI can configure a run without touching either.
10. As a CUDA-Q user, I want run-time arguments to take precedence over the
    environment, and the environment over the compile-time defaults, so that the
    override order is the one I expect.
11. As a CUDA-Q user, I want every argument I pass on the command line to be
    honoured, so that I am never silently running against a different endpoint
    than the one I named.
12. As a CUDA-Q user, I want a clear error when the QuAM state file is missing
    or unreadable, so that I fix a path instead of debugging a segfault.
13. As a CUDA-Q user, I want a clear error when the serialized QuaConfig is
    missing, so that I know which artifact to regenerate.
14. As a CUDA-Q user, I want a clear error when the Python builder fails, with
    its output preserved, so that I can see the Python traceback that caused it.
15. As a CUDA-Q user, I want a clear error when my circuit needs a two-qubit gate
    across a pair the chip does not have active, so that I learn it is a routing
    problem and not a pulse problem.
16. As a CUDA-Q user, I want a clear error when the QOP rejects my program, so
    that I can distinguish a compilation failure from a network failure.
17. As a CUDA-Q user, I want the quantum machine my run opened to be released
    when the run ends, so that repeated runs do not exhaust the instrument.
18. As a CUDA-Q user, I want the quantum machine released even when my run fails
    partway, so that a crash does not leave the instrument occupied.
19. As a CUDA-Q user, I want `cudaq::run` to keep failing with a clear message,
    so that I know it is genuinely unsupported rather than broken.
20. As a CUDA-Q user, I want a multi-term `cudaq::observe` to submit every term,
    so that my expectation values are not silently computed from one circuit.
21. As a physicist, I want to know which physical qubits my logical qubits landed
    on, so that I can correlate a result with the calibration data for those qubits.
22. As a physicist, I want repeated runs of the same circuit structure to reuse
    one QOP compile, so that a parameter sweep is not dominated by compilation.
23. As a physicist, I want the run to complete in a time comparable to the
    equivalent hand-written QUA script, so that using CUDA-Q is not a penalty.
24. As a target maintainer, I want the end-to-end path exercised by a test that
    enters through `cudaq::sample`, so that a defect between the platform layer
    and the executor cannot hide behind a lower-level test.
25. As a target maintainer, I want that test to run against a mock QOP by
    default, so that the suite is runnable without the instrument.
26. As a target maintainer, I want the mock to return known bitstrings, so that
    I can assert on exact decoded counts rather than on plausibility.
27. As a target maintainer, I want the same test binary to run against the real
    QOP behind an explicit flag, so that the offline and live paths cannot drift.
28. As a target maintainer, I want anything capable of queueing a job to require
    an explicit opt-in, so that a mistyped argument cannot fire pulses.
29. As a target maintainer, I want a failing assertion in a live test to unwind
    normally, so that cleanup runs and no quantum machine is left open.
30. As a target maintainer, I want the goldens reproducible from the QuAM state
    the project is actually pointed at, so that a regeneration is a no-op rather
    than a surprise.
31. As a target maintainer, I want the container invocation documented as one
    contract, so that each script does not rediscover the mount layout.
32. As a target maintainer, I want a missing protobuf at configure time to be
    loud, so that a build does not silently produce a target that falls back to a
    different code path.
33. As a target maintainer, I want the corpus to cover the circuit shapes the
    phases actually depend on, so that a passing suite means something.
34. As a maintainer reading this in six months, I want the reason the project
    moved back to the five-qubit chip recorded, so that nobody reverts it by
    reading only the older note.

## Implementation Decisions

### Chip and QuAM state

The target moves back to the five-qubit QuAM state. This reverses a decision
recorded earlier in the project's TODO, and the reversal is deliberate: the
ten-qubit state references a pulse class absent from the installed `quam_libs`,
so it cannot be loaded at all, on the host or in the container. The five-qubit
state references only pulse classes that are present, and was verified to build
a Bell program and to pass a live cluster healthcheck.

The five-qubit state is also structurally simpler for this phase: every qubit is
active, so the CUDA-Q logical index maps to the physical qubit of the same
ordinal with no offset, and every adjacent pair has an active coupler. The
offset hazard that existed on the ten-qubit state does not arise.

The older note in the project TODO forbidding this state must be amended rather
than deleted, and the reason for the reversal recorded next to it.

### The sample guard

The executor currently rejects a request when the execution context is `run`
**or** when a raw-output buffer pointer is non-null. The second condition is
wrong: the sample path always supplies that pointer, so the guard rejects every
sample. Only the execution-context condition survives. `cudaq::run` remains
unsupported and must continue to produce a clear message.

This defect was undetectable at the executor seam, because the executor's own
checks call it with a null pointer. That is the argument for the new test seam
below, and the reason the fix and the seam ship together rather than separately.

### Test-harness safety

Three findings from the P3 review are closed in this phase.

- The assertion helper used by the check programs terminates the process
  directly, which skips destructors and defeats the executor's own cleanup. It
  must instead record the failure and return a non-zero exit status through
  normal unwinding, so that the quantum machine opened by a live check is
  released even when an assertion fails.
- Both driver scripts append the caller's arguments after their own, and both
  argument readers take the first match. The caller's arguments must win. The
  fix is positional: the script's defaults go before the caller's arguments, not
  after. This applies to the existing P2 and P3 scripts as well as the new one.
- The P3 check program gates its job-queueing path on a log path being set
  rather than on an explicit mock opt-in. It must require the same explicit
  opt-in the P2 program already requires, so that no combination of arguments
  reaches the queue-a-job path without the operator having said so.

Two further findings are deliberately deferred, and the reasoning is recorded so
it is not rediscovered:

- The weak coverage of rotation-angle slot ordering is a P5 concern. The Bell
  circuit contains no rotation gates, so slot ordering cannot affect this phase.
- The result-count guard that may fire spuriously on a second fetch is left in
  place. It throws a clear message, and this phase is the first opportunity to
  learn what the instrument actually means by that count. Softening it in
  advance would discard the only signal available.

### Qubit mapping

The logical-to-physical mapping stays implicit: the builder indexes the chip's
active-qubit list, so logical ordinal N is the Nth active qubit. On the
five-qubit state this is the identity.

A two-qubit gate whose mapped pair is not in the chip's active-pair list must
raise a clear error naming both the logical and the physical pair. Without it,
an unroutable circuit fails as a pulse-level malfunction rather than as a
routing message. Explicit pinning of logical to physical qubits is deferred to
P5, where circuit topology starts to matter.

### Phase criterion

The criterion for this phase is decode correctness, not fidelity. A pass
requires that counts are returned, that they decode into the declared classical
register, that the register width matches the number of measured qubits, and
that the counts sum to the requested shot total. The observed proportion of
correlated outcomes is recorded as an observation, not asserted.

This is deliberate: the phase validates this repository's software, not the
instrument's calibration. Asserting a fidelity threshold would make the phase
fail for a reason nothing in this repository can fix.

### Container execution contract

Everything runs inside the project's devcontainer image. Host-native execution
is not viable: the container's C library is newer than the host's, so
container-built shared objects cannot load on the host at all, and the host has
neither the compiler nor an installed CUDA-Q.

Three properties of the container invocation are load-bearing and must be
treated as one contract shared by every script:

- The repository must be mounted at the path the compiler driver hard-codes when
  it detects a development-tree build. The existing scripts use a different
  mount point, which works for their own binaries but fails for the driver.
- The user's external Python tree must be mounted at its own absolute path. The
  library the builder imports is an editable install whose finder hard-codes
  absolute paths, so any other mount point resolves the interpreter and the
  published packages correctly but fails on that one import.
- The protobuf runtime is required at run time, not only at compile time, and is
  not present in the image. Baking it into the image is preferred to installing
  it on every invocation; this also removes the real-world trigger for the
  silent-configure problem below.

### Build configuration

A missing protobuf at configure time currently causes the whole target to be
skipped with only an informational message, after which the runtime falls back
to a different executor whose job-creation path submits only the first circuit
of a multi-circuit request. The configure-time skip must be loud enough that a
developer notices, since the failure mode is silent wrong behaviour rather than
a build error.

### Vocabulary

Three words in this project each denote two things, and the ambiguity has
already caused confusion in the project's own notes. The spec uses, and the
implementation should adopt, the disambiguated forms: *compile* for the C++
toolchain only and *QOP compile* for the instrument's remote compilation;
*gate* for a quantum gate only and *phase criterion* for a phase's pass
condition; *qubit* for the CUDA-Q logical index, *physical qubit* for the named
chip qubit, and *active index* for a position in the chip's active-qubit list.

A root context glossary recording these should be created as part of this
phase. Existing notes are corrected opportunistically as they are touched, not
in a single sweep.

## Testing Decisions

### What a good test is here

A good test in this area asserts on externally observable behaviour: the counts
a user's binary prints, the sequence of remote procedure calls the instrument
observed, and the bytes on the wire. It does not assert on the executor's
internal cache state, on the shape of intermediate data structures, or on the
builder's internal variable naming.

Two properties specific to this codebase matter more than usual:

- **Assert from the receiving side, not the sending side.** The P3 review found
  that the strongest evidence for the compile-once behaviour was the mock's own
  recorded call log, not the check program's assertion about what it believed it
  had sent. Where a mock can record what it received, assert on that record.
- **Enter at the highest seam the behaviour is visible from.** The sample guard
  defect existed for the whole of P3 and was invisible because the checks called
  the executor directly. A test that enters below the layer where a defect lives
  cannot find it, however thorough it is.

### Seams

One new seam is added, and it is the highest available: **the compiled user
binary**. A minimal Bell kernel is compiled with the compiler driver against
this target, the resulting binary is executed, and the assertion is made on what
`cudaq::sample` returned. This is the only seam from which the platform layer,
the executor, the builder subprocess, the transport and the result decoder are
all exercised together, and the only one from which the sample guard defect is
visible.

It runs in two modes from one program: against the mock instrument by default,
and against the real instrument behind an explicit opt-in. The mock already
supports returning a caller-specified list of bitstrings, so the offline mode
asserts exact decoded counts rather than plausibility. The live mode is the
phase criterion.

Four existing seams are reused unchanged, and no new seam duplicates them:

- The builder's golden-corpus check, covering circuit-to-QUA-program translation
  offline, and QOP compilation of each corpus entry when run against the
  instrument.
- The transport check, covering the framing, trailers and connection reuse of
  the remote-procedure-call client against both a mock and the instrument.
- The executor check, covering the structural cache: that two requests differing
  only in rotation angles produce one QOP compile and two parameter pushes.
- The wire-compatibility check, comparing this project's hand-written protobuf
  schema against the vendor's generated descriptors field by field.

The executor check keeps its existing lower seam rather than being folded into
the new one. It asserts on call sequencing, which is cheaper and more direct to
provoke from the executor, and rewriting it would risk the compile-once evidence
that phase produced. The new seam's existence is the mitigation for what the
lower seam cannot see.

### Prior art

The two existing check programs are the model to follow: a single C++ program of
independent named checks, each reporting pass or fail, driven by a shell script
that starts the mock, runs the program, and tears the mock down. The new program
follows that structure, with two departures required by this phase: it invokes
the compiler driver as part of the check rather than being compiled by it, and
its assertion helper returns a status rather than terminating, per the fix above.

The golden-corpus regeneration script is the model for reproducibility: the
goldens are rebuilt from the QuAM state rather than edited, so that a chip
recalibration moves them by regeneration rather than by hand.

### What is tested

The new seam covers: that a sample request returns rather than throwing; that
the returned register name and width match the kernel's declaration; that the
counts sum to the requested shots; that the decoded bitstrings match the mock's
configured results exactly; that the run releases the quantum machine it opened;
and that a failing assertion still releases it.

The existing seams are re-run against the five-qubit state to confirm the
retargeting is complete, including regenerating the goldens and confirming the
regeneration is reproducible.

## Out of Scope

- **QAOA and the optimizer loop.** The parametric push path exists and is
  covered by the executor check, but converging an optimizer against the
  instrument is the next phase.
- **Rotation-angle slot ordering coverage.** Deferred to the phase where it can
  affect a result. The Bell circuit has no rotation gates.
- **A native C++ program emitter.** The Python builder subprocess remains, once
  per circuit structure.
- **Explicit logical-to-physical qubit pinning and routing.** Deferred; this
  phase adds only the error that fires when a mapping is unroutable.
- **The ten-qubit chip.** Returning to it requires resolving the pulse-class
  mismatch in the external Python tree, which is outside this repository.
- **Portability of run-time argument reading.** Arguments are read from a
  Linux-specific process interface and are a silent no-op elsewhere. The
  environment variable and the compile-time argument are the portable paths and
  are unaffected. Not addressed here.
- **The pre-existing REST-based path for this target name.** Its declared
  arguments are inherited and unused. They are left alone.
- **Fidelity, calibration, and any assertion about the instrument's physics.**

## Further Notes

- The reversal to the five-qubit chip contradicts a note in the project TODO
  written four days earlier. Both the note and the reason for its reversal must
  survive, or the next reader will revert it.
- The pulse-class mismatch that forced the reversal is in the user's external
  Python tree, which this project treats as a read-only reference. It is
  recorded here as a known external defect, not as work.
- The goldens currently checked in were produced from a revision of the
  ten-qubit state that no longer exists on disk. They are not reproducible as
  they stand. Regenerating them against the five-qubit state is part of this
  phase and will change every one of them.
- Opening a quantum machine pushes configuration to the controllers and queueing
  a job plays pulses. Both are real actions on shared laboratory hardware. This
  is why the mock opt-in is treated as a correctness requirement rather than a
  convenience.
