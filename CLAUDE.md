# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

CUDA-Q: a hybrid quantum-classical platform. This repo contains the `nvq++`
compiler (Clang frontend + MLIR dialects), the CUDA-Q runtime, the CPU/GPU
simulator backends, and the Python bindings — all built as one CMake project.

LLVM/MLIR is a submodule pinned to a specific commit (currently LLVM 22.1.4) and
must be built with that exact commit. Third-party deps live in `tpls/` as
submodules.

Deep dives on the subsystems that take the most reading to understand:

- `ARCHITECTURE-MLIR.md` — dialects, adding ops and passes, TableGen wiring,
  the three producers of Quake.
- `ARCHITECTURE-BACKEND.md` — NVQIR simulators, platform/QPU layer, remote REST
  QPUs, target YAML.
- `ARCHITECTURE-REALTIME.md` — NVQLink: `device_call` lowering, the RPC wire
  protocol, dispatch modes, transport/channel plugins.

## Build

The build is driven by `scripts/build_cudaq.sh`, which configures CMake + Ninja
into `build/` and installs into `$CUDAQ_INSTALL_PREFIX` (default `~/.cudaq`).
`scripts/set_env_defaults.sh` supplies the `*_INSTALL_PREFIX` defaults
(`/usr/local/*` on Linux, `~/.local/*` on macOS) and is sourced by the other
scripts.

```bash
bash scripts/build_cudaq.sh              # release build
bash scripts/build_cudaq.sh -v -c Debug -j 8
bash scripts/build_cudaq.sh -p           # also install prerequisites (incl. LLVM)
bash scripts/build_cudaq.sh -s           # ASan + UBSan
bash scripts/build_cudaq.sh -- -DCUDAQ_LIT_JOBS=2   # args after -- go to cmake
```

Incremental rebuilds are much faster done directly:

```bash
cd build && ninja            # or: ninja <target>
```

Notes:
- `LLVM_INSTALL_PREFIX` only matters at the initial CMake configure; afterwards
  it is cached in `CMakeCache.txt`.
- GPU components are auto-omitted if CUDA/cuQuantum are not found — a GPU is not
  required to develop here.
- Warnings are errors by default; disable with `CUDAQ_WERROR=OFF`.
- Useful CMake options: `CUDAQ_BUILD_TESTS`, `CUDAQ_DISABLE_RUNTIME` (compiler
  only), `CUDAQ_DISABLE_CPP_FRONTEND`, `CUDAQ_SKIP_MPI`,
  `CUDAQ_TEST_MOCK_SERVERS`, `CUDAQ_ENABLE_SANITIZERS`.
- After a failed build, a rerun may reuse partial state and wrongly conclude a
  stage succeeded. Clean with `rm -rf build`, or reset the offending
  `*_INSTALL_PREFIX` / submodule build dir.

## Test

There are four distinct test suites; `scripts/run_tests.sh` runs all of them in
the same order CI does, with OpenMP/GPU-aware job budgeting.

```bash
bash scripts/run_tests.sh [-v] [-B build_dir] [-j N]
```

Individually:

```bash
# 1. gtest unit tests (unittests/, cudaq/unittests/) via ctest
cd build && ctest --output-on-failure
ctest -R <test-name>
ctest --label-exclude gpu_required     # on machines without a GPU

# 2. Compiler FileCheck tests (cudaq/test/)
"$LLVM_INSTALL_PREFIX/bin/llvm-lit" -v \
  --param cudaq_site_config=build/cudaq/test/lit.site.cfg.py build/cudaq/test
# or: cd build && ninja check-cudaq

# 3. End-to-end target tests (targettests/) — these actually compile and run
"$LLVM_INSTALL_PREFIX/bin/llvm-lit" -v \
  --param cudaq_site_config=build/targettests/lit.site.cfg.py build/targettests
# or: cd build && ninja check-targets

# 4. Python
PYTHONPATH=build/python python3 -m pytest -v python/tests/ --ignore python/tests/backends
for t in python/tests/backends/*.py; do python3 -m pytest -v $t; done
llvm-lit --param cudaq_site_config=build/python/tests/mlir/lit.site.cfg.py build/python/tests/mlir
```

Running one test:
- lit: pass the single `.cpp`/`.qke` file path instead of the directory.
- gtest: on macOS `ctest` registers one test per executable (not per method), so
  run the binary directly:
  `./unittests/nvqpp/test_ptsbe --gtest_filter='SomeTest.SomeCase'`.
  Configure with `-DCUDAQ_TEST_SPLIT_GTESTS=ON` for per-method ctest entries.

Where a new test belongs:
- Runtime library code → `unittests/` (gtest).
- Anything affecting compiler output → a FileCheck test in `cudaq/test/`.
- Full compile-and-run behavior across targets → `targettests/`.

## Format and lint

Pre-commit runs the same checks as CI. Fast hooks (clang-format-22, yapf Google
style, markdownlint) run on commit; license headers, pyspelling, and markdown
link checks run on push.

```bash
pip install pre-commit && pre-commit install
pre-commit run --all-files
pre-commit run --all-files --hook-stage pre-push   # includes spell/link checks
pre-commit run clang-format --all-files
```

Commits must be signed off (`git commit -s`) — unsigned commits are rejected.

## Compiler architecture (`cudaq/`)

`nvq++` (`cudaq/tools/nvqpp/nvq++.in`) is a bash driver, not a binary. It
orchestrates three tools and builds the MLIR pass pipeline as a string:

1. `cudaq-quake` — Clang-based frontend (`cudaq/lib/Frontend`); lowers `__qpu__`
   C++ kernels to the **Quake** dialect and emits the classical host code as
   LLVM IR in parallel.
2. `cudaq-opt` — runs the `--pass-pipeline` that `nvq++` assembles from the
   target config and flags (`cudaq/lib/Optimizer/Transforms`).
3. `cudaq-translate` — emits the transport-layer format via
   `--convert-to=<name[:version[:suboptions]]>`, where name is `qir` (default,
   `qir:0.1`), `qir-full`, `qir-adaptive`, `qir-base`, `openqasm2`, or `iqm`
   (`cudaq/lib/Optimizer/CodeGen`).

Two MLIR dialects, both in `cudaq/include/cudaq/Optimizer/Dialect`:
- **Quake** — quantum ops. Has two forms: *reference/memory* semantics
  (`quake.alloca`, ref-counted qubits) and *value/wire* semantics (SSA wires).
  `memtoreg` / `regtomem` convert between them; most optimizations want the
  value form.
- **CC** ("classical computation") — the classical control flow, aggregates, and
  data structures that survive alongside quantum ops.

`cudaq/lib/Optimizer/Transforms/Pipelines.cpp` defines the canonical named
pipelines (target-prep → target-deploy → target-finalize, plus the Python AOT
pipeline). **The pass sequence in `nvq++.in` and the `PythonAOT` pipeline must be
kept in sync** — there is a comment in `nvq++.in` marking this.

To inspect IR while working on the compiler:

```bash
cudaq-quake docs/sphinx/applications/cpp/grover.cpp
cudaq-quake docs/sphinx/applications/cpp/grover.cpp \
  | cudaq-opt --canonicalize --add-dealloc \
  | cudaq-translate --convert-to=qir
```

Other tools: `cudaq-opt` accepts custom pipelines for debugging;
`cudaq-target-conf` expands target YAML; `cudaq-lsp-server` for editor support.

## Runtime architecture (`runtime/`)

The execution path for a kernel call:

```
user kernel (qubit_qis.h)
  → ExecutionManager        runtime/cudaq/qis/execution_manager.h
  → NVQIR CircuitSimulator  runtime/nvqir/CircuitSimulator.h
  → backend (qpp, custatevec, cutensornet, stim, cudensitymat, ...)
```

- `runtime/cudaq/` — the user-facing API (`cudaq.h`, `algorithms/` for
  `sample`/`observe`/`run`/`evolve`/VQE, `qis/` for qubit types, `operators/`
  for spin/boson/fermion/matrix operators).
- `runtime/common/` — target-agnostic machinery shared by local and remote
  execution: `ExecutionContext`, `ServerHelper`/`Executor`/`RestClient` (REST
  QPU submission), `NoiseModel`, `SampleResult`, `BaseRemoteRESTQPU`.
- `runtime/nvqir/` — simulator backends. Register via the
  `NVQIR_REGISTER_SIMULATOR` macro; each builds a separately loadable plugin
  `.so` selected at runtime by the target config.
- `runtime/cudaq/platform/` — `quantum_platform` owns one or more `QPU`s.
  `default/` is single-QPU, `mqpu/` multi-QPU; vendor platforms (quantinuum,
  ionq, iqm, braket, quera, pasqal, orca, …) live alongside, mostly under
  `default/rest/helpers/`.

**Targets are YAML, not code.** Each `*.yml` under `runtime/cudaq/platform/`
(and next to simulators) declares the target name, GPU requirement,
`target-arguments`, and a `configuration-matrix` mapping option flags to an
`nvqir-simulation-backend`, preprocessor defines, and pass-pipeline overrides.
Adding or changing a backend usually means touching both the C++ and its YAML.

## Python (`python/`)

`python/cudaq/` is the Python package; `python/runtime/` holds the nanobind
bindings into the C++ runtime; `python/cudaq/kernel/` implements the
`@cudaq.kernel` AST→Quake translation (the Python frontend, independent of the
Clang frontend). MLIR-level Python tests are lit tests under `python/tests/mlir`;
everything else is pytest.

## Code style and API layers

Public APIs follow their language's style guide; **CUDA-Q internals follow the
LLVM/MLIR style guide**. In practice: `snake_case` for the user API, `CamelCase`
for compiler/internal code.

`CppAPICodingStyle.md` defines three layers — respect them when adding headers:

| Layer | Include path | Namespace | Stability |
|---|---|---|---|
| User API | `cudaq.h`, `cudaq/<subsystem>/<hdr>.h` | `cudaq::` (`cudaq::detail` is non-public) | backward compatible; breaking changes need a deprecation plan |
| Internal public module API | `cudaq_internal/<module>/<hdr>.h` | `cudaq_internal::<module>::` | shipped but not user-stable |
| Internal private | module-local (`src/`, `include-private/`) | `..::detail` | free to change |

Internal headers must not live under the `cudaq/` include root.

Every file needs the Apache-2.0 header comment (enforced by the `license-headers`
hook) and a comment describing its purpose.

## Debugging

```bash
CUDAQ_LOG_LEVEL=info|trace CUDAQ_LOG_FILE=out.txt ./my_kernel.out
```

## Repo-local skills

`skills/cudaq-guide` and `skills/qiskit-to-cudaq` are user-facing skills shipped
with the product (onboarding guidance and Qiskit porting). They document CUDA-Q
*usage*, not this codebase's internals — update them when user-visible APIs or
target behavior change.

## Agent skills

### Issue tracker

Local markdown under `.scratch/<feature>/`; there is no GitHub issue tracker for
this fork. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical roles, unrenamed. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` and `docs/adr/` at the repo root (neither exists
yet; they are created lazily). See `docs/agents/domain.md`.
