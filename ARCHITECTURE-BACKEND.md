# CUDA-Q Architecture Notes: The Backend Layer

Working notes on simulators and hardware backends — NVQIR, the platform/QPU
layer, remote REST QPUs, and target YAML. Companion to `ARCHITECTURE-MLIR.md`
(the compiler layer) and `CLAUDE.md` (build/test/lint commands).

Read from the source tree, not from running the code. Paths accurate as of the
`main` branch at commit `6e15b2a25c`.

## Two kinds of backend

"Backend" means two things, plugging in at different layers:

- **Simulation backends** (NVQIR) — `runtime/nvqir/`. Implement `CircuitSimulator`;
  a gate applied by a kernel lands here directly.
- **Hardware/remote backends** — `runtime/cudaq/platform/`. Implement `QPU`
  (usually via `BaseRemoteRESTQPU` + a `ServerHelper`); the kernel is compiled to
  QIR/QASM, serialized, and POSTed to a vendor REST API.

Local simulation dispatch chain:

```
kernel (qubit_qis.h)
  → ExecutionManager      runtime/cudaq/qis/execution_manager.h
  → CircuitSimulator      runtime/nvqir/CircuitSimulator.h
  → qpp / custatevec / stim / …
```

For hardware, `quantum_platform` owns a `QPU` that intercepts `launchKernel` and
never reaches NVQIR at all.

## Simulation backends

| Backend | Directory | Notes |
|---|---|---|
| `qpp-cpu`, `density-matrix-cpu` | `nvqir/qpp` | CPU state vector / density matrix, OpenMP-parallel |
| `custatevec-fp32/fp64` | `nvqir/custatevec` | GPU state vector; also mgpu (MPI) and mqpu variants |
| `tensornet`, `tensornet-mps` | `nvqir/cutensornet` | Tensor network |
| `dynamics` | `nvqir/cudensitymat` | Open-system evolution; Runge-Kutta, Magnus, Crank-Nicolson integrators |
| `stim` | `nvqir/stim` | Clifford/stabilizer |
| `dem`, `resourcecounter` | `nvqir/dem`, `nvqir/resourcecounter` | Detector error models; gate counting instead of simulation |

### Adding one

Deliberately small. Subclass `CircuitSimulatorBase<ScalarType>` — the template
parameter is how fp32/fp64 variants are generated — and implement roughly five
methods: `addQubitToState`, `deallocateStateImpl`,
`applyGate(const GateApplicationTask&)`, `measureQubit`, `sample`. The base class
handles qubit ID tracking, gate queueing/flushing, execution contexts, noise, and
mid-circuit measurement bookkeeping. `QppCircuitSimulator.cpp` is ~430 lines and
is the reference to copy.

Register and describe it:

```cpp
NVQIR_REGISTER_SIMULATOR(nvqir::QppCircuitSimulator<qpp::ket>, qpp)
```

The macro emits two `extern "C"` symbols — `getCircuitSimulator()` and
`getCircuitSimulator_qpp()` — both returning a `thread_local` singleton. The
suffixed one lets `AnalysisScope` `dlsym` a *specific* simulator by name while
another is active. Add a `<name>.yml` next to the source; the library builds as
`libnvqir-<name>.so`.

## Simulator selection: the C++/Python asymmetry

Worth knowing before debugging a target problem.

**In C++, the simulator is bound at link time.** The target YAML's
`nvqir-simulation-backend` key is compiled by `cudaq-target-conf` into a *shell
fragment* (`TargetConfigYaml.cpp` emits literal
`if [ -f "${install_dir}/lib/libnvqir-..." ]` text), which `nvq++` sources to set
`NVQIR_SIMULATION_BACKEND` and append `-lnvqir-<backend>`. Default is `qpp`,
upgraded to `custatevec-fp32` if a GPU is found.

**In Python, it is `dlopen` at runtime.** `python/utils/LinkedLibraryHolder.cpp`
scans the lib directory for `libnvqir-*.so` to build the available-target list,
then `dlopen`s the selected one lazily. `cudaq.set_target()` swaps it live.

`getCircuitSimulatorInternal()` in `NVQIR.cpp` resolves in a fixed priority order:

1. `activeAnalysisSimulator` (an `AnalysisScope` is active)
2. an already-set `simulator`
3. `externSimGenerator` (set via `__nvqir__setCircuitSimulator`, used by Python)
4. `simulatorInitCallback` — lazy-init hook so Python can defer target setup
5. fallback: `dlopen(nullptr)` + `dlsym("getCircuitSimulator")`

`simulator` is `thread_local`, and `clone()` is what gives each thread its own
instance under MQPU.

## Hardware backends

Twelve vendors under `platform/default/rest/helpers/`: anyon, braket, infleqtion,
ionq, iqm, oqc, qbraid, qci, quantinuum, quantum_machines, scaleway, tii. Plus
platforms with their own directories for non-REST or analog models: `pasqal`,
`quera`, `orca`, `fermioniq`.

The REST path splits in two:

- **`BaseRemoteRESTQPU`** (`runtime/common/`) — vendor-independent. Runs the JIT
  pipeline from the target config, lowers to the configured `codegen-emission`
  format, drives submit/poll.
- **`ServerHelper`** — vendor-specific, ~10 virtuals: `createJob`, `extractJobId`,
  `constructGetJobPath`, `jobIsDone`, `processResults`, `getHeaders`, plus polling
  interval. `IonQServerHelper.cpp` is a good model.

Registration is one line at the bottom of the file:

```cpp
CUDAQ_REGISTER_TYPE(cudaq::ServerHelper, cudaq::IonQServerHelper, ionq)
```

`ServerHelper`, `QPU`, `Executor`, and `RemoteRuntimeClient` are all
`registry::RegisteredType<T>`. `runtime/common/Registry.h` is a hand-rolled,
LLVM-free drop-in replacement for `llvm::Registry<T>` — the linked-list
`Head`/`Tail` must be instantiated in exactly one TU via
`CUDAQ_INSTANTIATE_REGISTRY`, or cross-DSO registration silently breaks.

Every REST QPU supports `emulate=true`, which flips `isRemote()`/`isSimulator()`
and runs the target's gate set and lowering pipeline locally against a simulator.
That is what the `*_LocalEmulation_*.py` tests exercise without credentials.

## Target YAML is the real integration point

Adding a backend is usually more YAML than C++. `TargetConfig.h` defines the
schema. Interesting keys, from `quantinuum.yml`:

```yaml
config:
  platform-qpu: remote_rest              # which QPU subtype
  gen-target-backend: true
  link-libs: ["-lcudaq-rest-qpu"]
  jit-high-level-pipeline: "expand-measurements"
  jit-mid-level-pipeline: "quantinuum-gate-set-mapping"
  jit-low-level-pipeline: "func.func(combine-quantum-alloc,qubit-reset-before-reuse)"
  codegen-emission: qir-adaptive:0.1:int_computations
```

The target file — not code — decides the MLIR pass pipeline and QIR profile.
Quantinuum's shows `machine-config` overriding `codegen-emission` per machine
family (H-series → `qir-adaptive:0.1`; Helios → `1.0` with
`float_computations,output_log`). Simulator targets instead use a
`configuration-matrix` mapping option flags (`fp32,mgpu`) to an
`nvqir-simulation-backend` plus preprocessor defines.

## Testing backends

- `unittests/backends/` — gtest against real simulators (`QPPTester`,
  `StimTester`, `CuStateVecTester`); GPU ones carry the `gpu_required` ctest label.
- `python/tests/backends/` — one file per vendor against mock REST servers in
  `python/tests/utils/mock_qpu`. Both `scripts/run_tests.sh` and `Developing.md`
  run these **one file at a time**, because target state leaks across tests in a
  shared process.
- `targettests/<vendor>/` — lit tests compiling through each target's real pipeline.
- Configure with `-DCUDAQ_TEST_MOCK_SERVERS=ON` to enable the C++ mock-server tests.
