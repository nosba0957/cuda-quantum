# TODO — OPX1000 target: CUDA-Q drives QUA over gRPC

Goal: `nvq++ --target quantum_machines qaoa.cpp` produces a binary that, run
against a QuAM state, submits a QUA program to the QOP over gRPC and runs a
QAOA loop without putting the program back on the wire every iteration.

## Architecture

```
cudaq::sample(kernel, gamma, beta)                   [the produced binary]
  → quantum_platform → BaseRemoteRESTQPU
  → JIT Compiler.cpp → OpenQASM 2.0                  [per call, in process]
  → QMExecutor::execute()                            ← the only new box
      │
      ├ structural hash of QASM2 (angles blanked)
      │
      ├ MISS → popen(python qua_build.py) once       → parametric QuaProgram
      │        C++ gRPC: QmmService/OpenQuantumMachine(config blob)
      │                  QmService/Compile → program_id
      │                  QmService/AddCompiledToQueue → job_id
      │
      └ HIT  → C++ gRPC: JobService/PushToInputStream(gamma, beta)   ← hot loop
                         JobService/GetNamedResults(stream) → counts
  → sample_result
```

The program crosses the wire **once per circuit structure**. Every COBYLA step
after that is one tiny unary RPC. That is the cost
`opnic_dgx_opx/tests/grpc_profiling/grpc_network_profiling.py` was written to
measure, and this is the design that removes it without needing OPNIC.

Hookup is one line — `qpu_utils.cpp:59` picks up any `Executor` registered under
the target name:

```cpp
executor = cudaq::registry::isRegistered<cudaq::Executor>(qpuName)
               ? cudaq::registry::get<cudaq::Executor>(qpuName)
               : std::make_unique<cudaq::Executor>();
```

→ `CUDAQ_REGISTER_TYPE(cudaq::Executor, QMExecutor, quantum_machines)`.
No YAML change, no QPU subclass.

## Decisions (settled 2026-08-31, do not re-litigate)

| Question | Answer |
|---|---|
| Emitter | Python, **once per structure**, via a subprocess. C++ owns everything on the hot path. Native C++ emitter is P6, not a blocker. |
| Parameter transport | `JobService/PushToInputStream` (unary gRPC). Same compile-once model as the OPNIC path, minus the NIC. |
| Transport lib | protobuf-lite + libcurl HTTP/2 prior-knowledge. **No grpc++.** `cpr`/libcurl already vendored. |
| QuaConfig | Opaque pre-serialized blob from the QuAM state. Never built in C++. |
| QuAM state | `~/as_ntu_ncku/as-qpu-5q4c/` (`$QUAM_STATE_PATH`), q1..q5 + 4 couplers. |
| QOP host | `10.21.19.201:9514`, cluster `QPX1000_4`, QOP 3.6.2 (gateway 3.6.0-13160.a7d6ed8), `qm.grpc.v2.*`. 1 GbE. |
| Milestone | Bell pair end to end, then QAOA. |

## Established facts (verified 2026-08-31, don't re-derive)

### CUDA-Q side
- QASM2 is produced **at run time**, in process, by
  `runtime/internal/compiler/Compiler.cpp:543` (`getTranslation`). Hence
  `jit-mid-level-pipeline` in the YAML.
- `Executor::execute()` is virtual; the inherited `RestClient` member can go
  unused. Precedent: `BraketExecutor` drives the AWS C++ SDK with zero HTTP
  (`runtime/cudaq/platform/default/rest/helpers/braket/BraketExecutor.cpp:143`).
- Return finished counts, not job IDs: `std::async` yielding a `sample_result`,
  picked up by `future(std::future<sample_result>&&)` at
  `runtime/common/Future.h:75`.
- Target emits OpenQASM 2.0 (`quantum_machines.yml:24` `codegen-emission: qasm2`).
- Decomposition basis (`quantum_machines.yml:22`) — exactly these 11 + measure
  ever arrive: `h s t r1 rx ry rz x y z cx`.
- `cudaq::exp_pauli` decomposes cleanly → `rx, h, cx, rz`. Safe for QAOA cost
  layers.
- **No protobuf, no grpc anywhere in the tree.** `tpls/` has armadillo, cpr,
  eigen, ensmallen, fmt, googletest, json, llvm, nanobind, qpp, spdlog, Stim,
  xtensor, xtl. protobuf-lite is a new dep.
- Prebuilt `build/bin/*` needs GLIBC 2.36/2.38; this host has less. Run compiler
  tools in `ghcr.io/nvidia/cuda-quantum-devcontainer:cu12.6-gcc12-main` (present
  locally).
- Known upstream bug: `QuantumMachinesServerHelper::createJob` submits only
  `circuitCodes[0]`, silently breaking multi-term `observe`. Our executor
  iterates `codesToExecute` itself, so it sidesteps this.

### QM wire protocol
- QUA is **never sent as text**. `qm/api/v2/qm_api.py:250` builds
  `QmServiceCompileRequest(quantum_machine_id, high_level_program=<QuaProgram>,
  config=<QuaConfig>)` → `/qm.grpc.v2.QmService/Compile`, then
  `AddCompiledToQueue`. Stock protoc stubs, so `.proto` text is recoverable from
  the descriptor pool.
- `QuaProgram` (`qm/pb/inc_qua.proto`) fields: `1 config, 2 dynConfig, 5 script,
  6 compilerOptions, 7 resultAnalysis, 8 config_update`. 225 transitively
  reachable message types.
- Services `qm.grpc.v2.{QmmService, QmService, JobService}`. All unary **except**
  `JobService/{GetNamedResult, GetNamedResults, GetJobStatusUpdates,
  PullSamples}` — server-streaming.
- `qm.simulate()` is server-side (`SimulationApi(BaseApi[FrontendStub])`,
  `qm/api/simulation_api.py:101`). Even simulator work needs the QOP host.
- **Connection coordinates live in `~/as_ntu_ncku/as-qpu-5q4c/wiring.json`**, in a
  `network` block: host `10.21.19.201`, **port 9514**, cluster `QPX1000_4`.
  `state.json` has no `network` section and `load_user_config()` is all-None, but
  `wiring.json` does — read host/port/cluster from there.
  `QuantumMachinesManager(host=...)` alone **fails**: `DEFAULT_PORTS = (80,)`, and
  port 80 accepts TCP then drops the HTTP/2 handshake.
- Server is **QOP 3.6.2** (gateway build `3.6.0-13160.a7d6ed8`), `qm.grpc.v2.*`, capabilities
  include `__qop3` and `supports_api_v2`. The `frontend.proto` path is not in play.
- The `cluster_name` header is not required on 9514 for `GetVersion`. Whether the
  gateway routes `OpenQuantumMachine` by cluster is **unverified**.
- **Input-stream names are mangled.** `qm.utils.general_utils.create_input_stream_name`
  prefixes with `input_stream_`, so `declare_input_stream(name="gamma")` is addressed
  on the wire as `"input_stream_gamma"`. Wrong name = silent no-op, not an error.
- The SDK sends `x-grpc-service: gateway` on every call (`SERVICE_HEADER_NAME`).
- `protoc` is absent on the host and in the devcontainer image; `apt-get install
  protobuf-compiler libprotobuf-dev` inside the container works (protoc 3.21.12).
  The container has **no `pkg-config`** — link with `-lprotobuf-lite` directly.
- A non-lite well-known type (e.g. `google.protobuf.Int64Value`) fails at **link**
  time, not at protoc time — protoc 3.21.12 accepts it and generates fine, but the
  symbols live in full `libprotobuf`, so `-lprotobuf-lite` alone gives undefined
  references. Use a wire-identical local message. `Int64Value` at
  `GetNamedResultsRequest.outputs.range.{from,to}` is the **only** well-known type
  reachable from the six RPCs.

### BLOCKER for P2 — the transport decision is not buildable as configured

`scripts/install_prerequisites.sh:539` builds CUDA-Q's curl with
**`-DUSE_NGHTTP2=OFF`**. So `/usr/local/curl` (`$CURL_INSTALL_PREFIX`, wired in at
`scripts/set_env_defaults.sh:63` → `CMakeLists.txt:385-388,731`, which also sets
`CPR_FORCE_USE_SYSTEM_CURL`) has **no HTTP/2**:

```
libcurl 8.21.0   CURL_VERSION_HTTP2 set: NO   nghttp2_version: (null)
```

The P0 spike proved h2c against the **distro** libcurl 8.5.0, which is not what
CUDA-Q links. The cpr FetchContent fallback (curl 8.10.1, `tpls/cpr/CMakeLists.txt:297`)
sets no nghttp2 option either.

**DECIDED: flip it to `ON`.** Far smaller than grpc++, which drags in abseil, re2,
c-ares and **full** protobuf, undoing P0's protobuf-lite result. The flag is
flipped; **P2 must still make nghttp2 available to the static build and rebuild
`/usr/local/curl`** — the flag alone does nothing if nghttp2 is absent at configure
time. This is a local patch on a tracked upstream file: carry it across rebases.
The spike hard-fails on `features & CURL_VERSION_HTTP2`, so a miss breaks loudly.

### Found in P1 (verified by running, 2026-08-31)

- **`nvq++` emits QASM2 that Qiskit rejects.** For a size-1 qreg,
  `printClassicalAllocation` appends `[0]` when `size==1`
  (`TranslateToOpenQASM.cpp:76`), producing `measure var0 -> var2[0];`, and
  `qiskit.qasm2` fails with *"cannot resolve broadcast in measurement"*. Hits
  `rz_sweep` and `sx_chain`. `qua_build.py` normalizes it; **any future consumer
  must too.** This is an upstream CUDA-Q bug, worth fixing separately.
- **QASM2 loses rotation-angle precision** — `printParameters` uses `%e`-style
  6-decimal formatting (`rz(7.853982e-01)` for π/4), ~7 significant figures. The
  C++ must push full-precision doubles **from its own kernel arguments**, never
  re-parsed from the QASM.
- **`QuaProgram` embeds absolute source paths.** `qm._loc._get_loc()` stamps
  `File "<abs path>", line N: <source>` on every statement with no off-switch.
  Stripping them cut `qaoa_p1` from **216840 → 15111 bytes (~14x)** and bell from
  58.7 KB → 4.3 KB; the QOP compiles fine without them. Matters for P2/P3 wire
  sizing. `qua_build.py --keep-loc` opts back in.
- **The qm SDK logs to stdout on import** (`Starting session: <uuid>`), which
  corrupted the serialized protobuf on stdout — 97 bytes that still parsed.
  `qua_build.py` dups fd 1 aside before importing. Any subprocess contract that
  puts binary on stdout must do the same.
- **`QuaConfig` serialization is not byte-deterministic** (protobuf map ordering);
  messages compare equal but bytes differ per run. `QuaProgram` **is**
  deterministic.
- **`cz` never survives the target pipeline.** `quantum_machines.yml`'s basis ends
  in `x(1)` — controlled-X only — so `quake.z [ctrl]` lowers to `h/cx/h`.
  `cz_pair.qasm` is real emitter output generated *without* the decomposition
  pass, so the corpus still exercises the transpiler's cz path.
- **Bug in the fork's `design_qua_program_from_qiskit`**: `if target_qubits is not
  None: target_qubits = [machine.active_qubits[i] for i in range(circuit.num_qubits)]`
  silently overwrites the caller's qubit selection with the first N. `qua_build.py`
  implements its own builder and honors `--qubits`. Read-only — not ours to fix.
- **`cudaq-quake` segfaults** in `QPUCodeFinder::VisitVarDecl` on
  `runtime/cudaq/cudaq_mpi.h` — likely a prebuilt-binary vs `releases/v0.15.0`
  headers mismatch. Corpus `.qke` inputs are hand-written to route around it.
  Worth a separate look; does not block.

### Found in P2 (verified by running, 2026-08-31)

- **libcurl DOES deliver stream frames incrementally.** Proven, not inferred: with
  the mock spacing 4 frames at fixed delays, the C++ write callback saw them at
  exactly that cadence at 0.4s / 0.5s / 0.7s spacings. Buffering to completion would
  cluster all four arrivals within a millisecond. **The last unproven transport
  assumption in the design is now settled.**
- **`scripts/bootstrap_prerequisites.sh` is a near-duplicate of
  `install_prerequisites.sh` and also had `USE_NGHTTP2=OFF`.** A build going through
  the bootstrap path would still have produced an HTTP/2-less curl. Both are fixed —
  patch both on every rebase, not just the one.
- curl + nghttp2 is **contained**: build nghttp2 1.64.0 static
  (`ENABLE_LIB_ONLY=ON`, `BUILD_SHARED_LIBS=OFF`) into `$CURL_INSTALL_PREFIX`, then
  pass curl `NGHTTP2_INCLUDE_DIR` / `NGHTTP2_LIBRARY` / `NGHTTP2_USE_STATIC_LIBS`.
  One tarball, ~2 min. `CURL_ROOT` propagates into `CURLConfig.cmake`'s nested
  `find_dependency(NGHTTP2)`, so no extra CMake wiring. **grpc++ fallback not needed.**
  Local image `cuda-quantum-devcontainer:qm-http2` has it baked in.
- **The C++ client's requests are byte-identical to a Python SDK-descriptor client's**
  for the same inputs (71765 / 4357 / 39 / 60 / 27 bytes, same program digest).
  Independent validation of `qm_min.proto`'s opaque-`bytes`-for-message substitution.

**Two gaps that still matter:**

- **Result element width is an ASSUMPTION.** The `data` bytes on `GetNamedResults`
  are a raw numpy buffer with **no header**; `simple_dtype` and `shape` come from
  `GetJobNamedResultHeader`, which is **not** among the six RPCs in `qm_min.proto`.
  "Length-`creg.size` int buffer per shot" is right about content, but the width is
  defaulted to 4-byte LE (QUA `int` is 32-bit) and exposed as a constructor arg.
  **P3 decision: add `GetJobNamedResultHeader` as a seventh RPC, or accept the
  assumption and confirm at P4 against real data.** This is the one thing in
  `QuaResults` that could not be verified offline.
- **Bitstring ordering is a trap.** LSB-first packing is right, but the reference
  Python then formats via `bin(n).zfill(size)` — **MSB-first**, the Qiskit
  convention. CUDA-Q's `sample_result` strings are **qubit-0-first**
  (`QppCircuitSimulator.cpp:378`, ascending `measuredBits`). The C++ must not copy
  the Python's string formatting. `QuaResults` emits clbit-0-first and asserts it.
- `GetNamedResults` has a **chunk/summary envelope** not previously recorded:
  `DataChunk` frames accumulate per output name, a terminating `DataSummary` carries
  the count, and **chunk boundaries do not align to shot boundaries.**

### P1 verifier findings — act on these in P2/P3

- **BUG in `qua_build.py:332`** (TODO previously said 337). **FIXED in P2.** `"angle_values"` writes the full pre-transpile
  list while `"angles"` (line 333) lists only surviving parameters. They diverge
  whenever transpile eliminates a rotation — demonstrated at `--optimization-level 2`:
  `input_stream_size: 1, angles: ["theta_0"], angle_values: [0.3, 0.7]`. C++ zipping
  or pushing `angle_values` would push 2 floats into a size-1 stream. Not triggered
  at the default O1 by this corpus. Fix:
  `[values[int(p.name.split("_")[1])] for p in params]`.
- **The structural cache key needs more than blanked angles.** `shots` and
  `iterations` are compiled into the program, as are the QuAM calibration constants.
  Hashing only "QASM2 with angles blanked" collides across two `cudaq::sample` calls
  with different shot counts and reuses a program that runs the wrong number of
  shots. Key must include shots, iterations, optimization level, reset
  type/attempts, and a QuAM state fingerprint.
- **C++ must fold angles into (-π, π] before pushing.** `expr_to_qua` wraps only the
  constant term; the runtime term is `const + c·v`. QUA `fixed` is 4.28, so |value|
  must stay under 8 — with `const = -π`, a pushed angle ≳ 4.86 rad overflows, and
  COBYLA will happily propose γ outside (-π, π].
- **"Never re-parse angles from the QASM" is not implementable and should not be
  attempted.** `parameterize` gives *every* rotation its own slot — `qaoa_p1` has 2
  logical QAOA parameters but 9 stream slots. Mapping slot→kernel-argument is only
  possible from the QASM angle values. Re-parsing is fine: worst-case error is
  5e-7 rad ≈ 21 LSB of QUA `fixed`, physically negligible.
- **Result segmentation has no delimiter.** `save_all` accumulates across the whole
  `iterations` loop with nothing marking a boundary. `QuaResults` must count
  consumed buffers per push (`shots` each) to attribute results to a COBYLA step.
- **`cz_pair` stops testing `cz` above O1** — qiskit's
  `RemoveDiagonalGatesBeforeMeasure` legitimately elides the trailing CZ. Correct
  for sampling, but the fixture's coverage evaporates, and the corpus is P6's
  byte-exactness oracle.

### C++ contract fixed by P1

- **Input stream:** `declare_input_stream("client", "angles", fixed, size=N)` →
  wire name **`input_stream_angles`**, pushed via `PushToInputStream` with
  `fixed_stream_data`, a length-`N` float array per batch.
- **Always one push per batch, even when N=0** — a circuit whose angles all fold
  to constants still gets a 1-element trigger stream, so C++ never special-cases.
- `iterations` (default 1024) bounds how many batches one job serves.
- **Angle ordering:** `theta_k` is the k-th rotation angle in QASM instruction
  order — the same order C++ gets free when blanking angles for the structural hash.
- **Result buffer:** `resultAnalysis` = `saveAll("<creg>", buffer(<size>,
  map(booleancast, @re 0 r1)))`. `GetNamedResults` on the creg name yields one
  length-`creg.size` int buffer per shot, bit `i` = clbit `i`, **LSB-first** when
  packing (`sum(bits[i] << i)`).
- **Creg names are nvq++-generated** (`var3`, `var2`, `var6`) and not stable
  across kernels — take them from the manifest, never hardcode.
- Goldens depend on the QuAM **calibration**, not just the config: CZ phase
  compensations land as literals (`frame_rotation_2pi(0.641065134208185, 'q1.xy')`).
  Recalibration invalidates them; `tests/regen_goldens.sh` rebuilds.

### The existing Python stack (the user's, already working)
- **The Qiskit→QUA transpiler is at
  `~/as_ntu_ncku/qua-libs/.../quam_libs/experiments/qiskit_circuit/qiskit_to_qua.py`**
  (their `asqum/qua-libs` fork, branch `as_quam_qualibrate`).
  **The `qua-libs/` inside this repo is vanilla upstream and does NOT have it.**
  Point at the fork.
- API: `create_target(machine)`, `qiskit_to_qua_macro(circuit, machine,
  target_qubits, optimization_level)`, `design_qua_program_from_qiskit(circuit,
  machine, target_qubits, n_shots, optimization_level)`,
  `run_qua_program_and_return_results(...)`.
- `qiskit_to_qua_macro` is only ~60 lines of dispatch. All real gate→pulse
  knowledge lives in **QuAM macros**: `qubit.apply(name, *params)`,
  `qubit_pair.apply(name, *params)`, `qubit.align()`. Porting the emitter to C++
  means reimplementing that macro layer, `active_reset` retry loop included.
  That is why it is P6.
- Basis already in use: `['rz', 'sx', 'x', 'cz', 'measure', 'reset']`.
- Result shape: `stream.boolean_to_int().buffer(creg.size).save_all(creg.name)`
  — so `GetNamedResults` on the creg name yields one length-`creg.size` int
  buffer per shot. That is exactly what the C++ result parser must decode.
- `opnic_dgx_opx/qaoa_with_opt.py` is the **parametric** reference:
  `create_qaoa_circuit_parametric` (qiskit `Parameter` objects),
  `qiskit_to_qua_macro_parametric`, `build_qua_program`. Its docstring:
  *"No recompilation is needed between COBYLA steps — the angles are updated at
  runtime via frame_rotation."* It feeds angles through
  `declare_input_stream("opnic", ...)`; **we swap that transport for a plain
  `declare_input_stream`, fed by the `PushToInputStream` RPC.**
- `experiments-ethernet/qaoa_with_opt.py` is the slow variant — rebuilds and
  resubmits the whole program per COBYLA step. That is the cost we are removing.

### QuAM state `as-qpu-5q4c`
- `~/as_ntu_ncku/as-qpu-5q4c/{state.json,wiring.json}`, `$QUAM_STATE_PATH`.
  Root class `quam_libs.components.quam_root.FEMQuAM` (OPX1000, LF-FEM + MW-FEM).
- q1..q5, pairs `coupler_q1_q2 … coupler_q4_q5` — a 5-qubit chain, so the
  coupling map is linear. Transpilation must route to it.
- Refreshed by `~/as_ntu_ncku/fetch_5q_state.sh` from the `asqum/qua-libs` fork.

## Layout

```
runtime/cudaq/platform/default/rest/helpers/quantum_machines/
  QMExecutor.cpp          # Executor override, structure cache, gRPC calls
  GrpcCurl.cpp/.h         # unary + server-stream gRPC over libcurl h2c
  QuaResults.cpp/.h       # GetNamedResults buffers -> cudaq::sample_result
  qm_min.proto            # hand-cut subset: qm_api + qmm_api + job_api
  tools/
    dump_protos.py        # descriptor pool -> .proto text (one-shot reference)
    quam2pb.py            # QuAM state -> qua_config.pb
    qua_build.py          # QASM2 -> parametric QuaProgram bytes (subprocess entry)
  tests/corpus/*.qasm     # bell, rz sweep, sx chain, cz pair, qaoa p=1
  tests/corpus/*.golden.pb
```

## Working without the instrument

The OPX is **not available for test jobs**. Only `AddCompiledToQueue` and anything
downstream of it actually plays pulses; `GetVersion`, `OpenQuantumMachine` and
`Compile` are non-disruptive and P1 already ran all three against the live QOP.

| Blocked on instrument time | Proceeds now |
|---|---|
| `AddCompiledToQueue` | curl + nghttp2 rebuild |
| `GetNamedResults` on real data | `GrpcCurl` unary — verify live, compile-only |
| `PushToInputStream` to a live job | `GrpcCurl` streaming — against the mock |
| P4 Bell counts, P5 QAOA loop | `QuaResults` decoder — synthetic buffers |
| | `QMExecutor` + structure cache |
| | P3's RPC-log gate — fully mockable |
| | the three P1-verifier bug fixes |

**The enabling piece is a mock QOP** speaking the six RPCs, built on the real
descriptors. Precedent: `utils/mock_qpu/quantum_machines/` is the HTTP mock for the
cloud target; this is its gRPC sibling.

Note the one genuinely open question — *does libcurl's write callback deliver gRPC
frames incrementally, or buffer the response to completion?* — is a question about
**libcurl**, not the QOP. A mock emitting N frames with delays answers it fully.
Hardware would answer it no better.

Keep the endpoint configurable so the same tests run against the mock and, when the
instrument frees up, against the real QOP unchanged.

## Phases

Each phase: **agent 1 builds, agent 2 verifies against the stated gate.** The
verifier reports, it does not fix; failures come back for re-dispatch.

### P0 — schema + transport spike — **PASSED** (verified independently)
- [x] `dump_protos.py` → 14 `.proto` files, 3889 lines. Reproducible: a rerun
      diffs identical against the checked-in tree.
- [x] `qm_min.proto`, 6 RPCs. **Field-number audit: 37 message pairs, 6 method
      paths, 0 divergences** — driven structurally from the real service
      descriptors, not by name matching.
- [x] h2c prior-knowledge reaches `10.21.19.201:9514`, `grpc-status: 0`,
      `HTTP/2 200`. Works on both devcontainer libcurl 8.5.0 and host curl 7.81.
- **Gate: PASSED.** protoc 3.21.12 compiles clean; `g++ -Wall -Wextra` builds it
  warning-free; links against `-lprotobuf-lite` alone with zero undefined
  `Descriptor`/`Reflection` symbols; live `GetVersion` returns the version string.

**Two P0 defects to fix before P3:**
- [ ] `QmServiceCompileRequest.high_level_program` (#2) and
      `OpenQuantumMachineRequest.config` (#1) are message-typed upstream
      (explicit presence) but plain `bytes` in `qm_min` (implicit presence). An
      **empty** value encodes differently — server sees "absent" where the SDK
      sends "present, zero-length". Non-empty values are identical, so the
      practical risk is near zero, but make them `optional bytes` for
      consistency (`QmServiceCompileRequest.config` #3 already is).
- [ ] `tests/run_spike.sh:18` interpolates `$*` unquoted into a `bash -lc`
      string. Cosmetic.

### P1 — Python side
- [ ] `quam2pb.py`: load QuAM from `$QUAM_STATE_PATH` → `machine.generate_config()`
      → serialize `QuaConfig` → `qua_config.pb`.
- [ ] `qua_build.py`: QASM2 on stdin → `qasm2.loads` → `transpile(target=create_target(machine),
      basis=['rz','sx','x','cz','measure','reset'])` → parametric QUA program
      (port `build_qua_program`, swapping `declare_input_stream("opnic", …)` for
      the plain gRPC-fed form) → `prog.qua_program.SerializeToString()` on stdout.
- [ ] Corpus + goldens: bell, rz sweep, sx chain, cz pair, QAOA p=1.
- **Gate:** each golden reloads via `QuaProgram.FromString`, and `qm.compile()`
  against the live QOP accepts it.

### P2 — gRPC client — **PASSED** on parts 2 and 3; part 1 deferred
- [ ] **Rebuild `/usr/local/curl` with nghttp2.** `USE_NGHTTP2=ON` is already
      flipped at `install_prerequisites.sh:539`, but the flag is inert unless
      nghttp2 is present at configure time. Confirm with a runtime probe that
      `CURL_VERSION_HTTP2` is set on the library CUDA-Q actually links — not the
      distro one, which already has it and misled the P0 spike.
- [ ] `tools/mock_qop.py` — a mock QOP speaking the six RPCs on the real
      descriptors, with an **RPC log** callers can assert against, and a
      `GetNamedResults` that emits N frames with configurable delays.
      Precedent: `utils/mock_qpu/quantum_machines/` (the HTTP mock for the cloud
      target). Endpoint must be configurable so the same tests point at the real
      QOP later, unchanged.
- [ ] `GrpcCurl`: unary call + server-stream frame reader over HTTP/2 h2c.
      Frame = `[0x00][4-byte BE length][message]`; check the `grpc-status`
      trailer. Handle the **trailers-only** error shape P0 found: `HTTP/2 200`,
      zero-length body, `grpc-status` in the header block rather than trailers.
- [ ] `QuaResults`: decode `GetNamedResults` buffers into `sample_result`.
      Contract is fixed and recorded above — length-`creg.size` int buffer per
      shot, bit `i` = clbit `i`, LSB-first. Count `shots` buffers per push to
      segment iterations; there is no delimiter in the stream.
- **Gate (achievable without the instrument):**
  1. A C++ binary does `OpenQuantumMachine(qua_config.pb)` + `Compile(bell.golden.pb)`
     against the **live QOP** and gets a program_id. Non-disruptive — P1 did
     exactly this from Python. **Do not call `AddCompiledToQueue`.**
  2. Against the mock, `GetNamedResults` returning 3 frames with delays is read
     **incrementally** — prove libcurl does not buffer to completion. This is the
     last unproven transport assumption, and hardware would not answer it better.
  3. `QuaResults` decodes synthetic buffers to the expected counts.
- **Deferred to instrument time:** submitting a job and reading real counts.

### P3 — QMExecutor
- [ ] `QMExecutor : cudaq::Executor`, override `execute()`, register it.
- [ ] Structural hash of QASM2 with rotation angles blanked → program cache.
- [ ] Miss → `qua_build.py` subprocess + `Compile` + `AddCompiledToQueue`.
      Hit → `PushToInputStream`.
- [ ] Iterate all of `codesToExecute` (see the `createJob` bug above).
- [ ] CMake: protobuf-lite; target arg for the `qua_config.pb` path.
- **Gate:** two `cudaq::sample` calls differing only in rotation angles produce
  **one** `Compile` RPC and **two** `PushToInputStream` RPCs. Assert on the RPC
  log, not on timing.

### P4 — Bell end to end
- [ ] `nvq++ --target quantum_machines bell.cpp && ./a.out --qm-config qua_config.pb`
- **Gate:** `cudaq::sample` returns counts from the QOP.

### P5 — QAOA
- [ ] Prefer `cudaq::sample` + classical cost over `cudaq::observe` — the cost
      Hamiltonian is diagonal, and it dodges the multi-circuit path entirely.
- **Gate:** COBYLA loop converges, and the RPC log shows one `Compile` for the
  whole optimization.

### P6 — native C++ emitter (optional endgame)
- [ ] Port QuaProgram construction + the QuAM macro layer to C++, deleting the
      `qua_build.py` subprocess.
- **Gate:** byte-exact against the P1 goldens on every corpus file.
      **`QuaProgram` is byte-deterministic (verified over 6 runs), so this holds.
      `QuaConfig` is NOT** — protobuf map ordering varies per run, so never hash or
      diff `qua_config.pb` bytes; compare parsed messages instead.

## Constraints for agents

- **Never write outside** `/home/asrlabncku/as_ntu_ncku/experiments/cuda-quantum`.
  `~/as_ntu_ncku/{qua-libs,as-qpu-5q4c,experiments-ethernet,experiments/opnic_dgx_opx}`
  and the venv are **read-only references**.
- Use `/home/asrlabncku/as_ntu_ncku/.venv/bin/python` for anything importing
  `qm`/`qiskit`/`quam`. System python3 has a broken numpy.
- The `qua-libs/` inside this repo is the **wrong branch** — use the fork at
  `~/as_ntu_ncku/qua-libs` for `quam_libs.experiments.qiskit_circuit`.
- Anything touching `build/bin/*` runs in the devcontainer image.
- The `Bash` working directory persists between calls — `cd` back to the repo root.

## Open items for the user

- [x] ~~QOP version on `10.21.19.201`~~ — **QOP 3.6.0, `qm.grpc.v2.*`, port 9514.**
      Resolved in P0 against the live instrument.
- [x] ~~Is the OPX free for test jobs?~~ **No — not available as of 2026-08-31.**
      Work is reordered around it: see "Working without the instrument" below.
- [ ] Ping when the OPX frees up — P4/P5 and the streaming-against-real-data checks
      are the only things waiting on it.
