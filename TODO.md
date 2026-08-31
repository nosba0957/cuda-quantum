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
| QOP host | `10.21.19.201`, 1 GbE. QOP version still to confirm. |
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
- No QMM host is stored anywhere: `load_user_config()` returns all-None and
  `state.json` has no `network` section. QuAM's `machine.connect()` supplies it.

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

## Phases

Each phase: **agent 1 builds, agent 2 verifies against the stated gate.** The
verifier reports, it does not fix; failures come back for re-dispatch.

### P0 — schema + transport spike
- [ ] `dump_protos.py`: descriptor pools of `qm.grpc.qm.grpc.v2.{qm_api,
      qmm_api,job_api}_pb2` → `.proto` text.
- [ ] Hand-cut `qm_min.proto`: the 5 RPCs we call plus their request/response
      messages. `high_level_program` and `config` are declared **`bytes`** —
      identical wire format, so QuaProgram/QuaConfig schemas are never needed.
- [ ] Confirm devcontainer libcurl has nghttp2 and h2c prior-knowledge reaches
      `10.21.19.201`.
- **Gate:** `protoc --cpp_out` compiles `qm_min.proto`; a C++ `QmmService/GetVersion`
  call against the live QOP returns a version string. If h2c fails, fall back to
  grpc++ and say so explicitly.

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

### P2 — gRPC client
- [ ] `GrpcCurl`: unary call + server-stream frame reader over HTTP/2 h2c.
      Frame = `[0x00][4-byte BE length][message]`; check the `grpc-status`
      trailer.
- [ ] `QuaResults`: decode `GetNamedResults` buffers into `sample_result`.
- **Gate:** a standalone C++ binary opens a QM from `qua_config.pb`, submits a
  golden Bell program, and prints counts. No CUDA-Q involved yet.

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

- [ ] QOP version on `10.21.19.201` (2.x vs 3.x) — decides `qm.grpc.v2.*` vs the
      older `frontend.proto` service. Blocks the final shape of `qm_min.proto`.
- [ ] Is the OPX free for test jobs during this work, or does it need booking?
