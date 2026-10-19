# CUDA-Q on a Quantum Machines OPX1000

The `quantum_machines` target runs CUDA-Q kernels on a bare OPX1000 through its
QOP server. It speaks QOP gRPC directly. QM's cloud service is not involved.
Words such as *variational parameter*, *circuit structure*, and *parameter
transport* are defined in [`GLOSSARY.md`](../../../../../../../GLOSSARY.md).

## How a job is submitted

```text
 cudaq::sample(...) or cudaq::max_cut(n, seed)
        |
 CUDA-Q JIT: kernel -> Quake -> parameter-table pass -> OpenQASM 2
        |    (a std::vector<double> argument stays open as p0, p1, ...)
        |
 QMExecutor (this directory)
        |  new circuit structure? -> tools/qua_build.py (Python subprocess):
        |     qiskit transpile -> QuAM gate macros -> QUA program
        |
 QOP over gRPC: OpenQuantumMachine, Compile, AddCompiledToQueue
        |
 parameters in, bits out, by the chosen parameter transport
```

| Transport | How to select | Compiles | Parameters in | Bits out |
|---|---|---|---|---|
| Ethernet | default | one per new parameter set | baked into the program | gRPC `GetNamedResults` |
| Streamed | `--qm-parametric 1` | one per circuit structure | gRPC input stream | gRPC `GetNamedResults` |
| OPNIC | `--qm-transport-opnic` | one per circuit structure | OPNIC card | OPNIC card |

Streamed is blocked on this QOP: its input streams accept pushes but the
program never advances. Use Ethernet or OPNIC.

## One-time setup

1. Build CUDA-Q in the devcontainer image `cuda-quantum-devcontainer:qm-http2`.
   The OPNIC plugin `libcudaq-qm-opnic.so` is built only when QM's opnic SDK
   is found (`/usr/include/opnic`, `/usr/include/qm`, `libopnic.so`). It needs
   GCC 13 or newer.
2. Make a Python environment with `qm-qua`, `qiskit`, and `quam`. Point
   `--qm-python` at it.
3. Write the chip's QuaConfig from its QuAM state, once per calibration:

   ```bash
   python tools/quam2pb.py -o qua_config.pb --state-path ~/as_ntu_ncku/as-qpu-5q4c
   ```

   This also writes `qua_config.pb.caps.json` next to it.

## Compile

```bash
nvq++ --target quantum_machines --quantum_machines-url unused my_program.cpp -o my_program
```

Every run-time setting below can also be fixed at compile time, for example
`--quantum_machines-qm-transport opnic`.

## Run

```bash
./my_program \
  --qm-endpoint 10.21.19.201:9514 --qm-cluster QPX1000_4 \
  --qm-config qua_config.pb \
  --qm-builder tools/qua_build.py \
  --qm-python ~/as_ntu_ncku/.venv/bin/python \
  --qm-state ~/as_ntu_ncku/as-qpu-5q4c
```

Add `--qm-transport-opnic` to use the OPNIC card. In a container, OPNIC also
needs `--device /dev/opnic0`, `-v /run/opnic:/run/opnic`, and `libopnic.so`
mounted. `tests/run_p5_checks.sh` shows the full `docker run` line.

Each setting is looked up in this order: the command-line flag `--qm-<key>`,
then the environment variable `CUDAQ_QM_<KEY>`, then the compile-time target
argument.

| Key | Default | Meaning |
|---|---|---|
| `endpoint` | target `url` | QOP gRPC `host:port` |
| `cluster` | | QOP cluster name |
| `config` | | serialized QuaConfig from `quam2pb.py` |
| `caps` | `<config>.caps.json` | server capabilities file |
| `builder` | `qua_build.py` | path to `tools/qua_build.py` |
| `python` | `python3` | Python that runs the builder |
| `state` | `$QUAM_STATE_PATH` | QuAM state directory |
| `tmpdir` | system temp | where each program's QASM, manifest, and `.pb` are kept |
| `transport` | `ethernet` | `ethernet` or `opnic` (`--qm-transport-opnic` is the same as `opnic`) |
| `parametric` | `0` | `1` selects the streamed transport |
| `iterations` | `1024` | evaluations one queued job serves before it is requeued |
| `opt-level` | `1` | qiskit transpiler optimization level |
| `reset` | `active` | `active` or `thermalize` |
| `reset-attempts` | `1` | active-reset retries |
| `opnic-in`, `opnic-out` | `987`, `654` | OPNIC stream ids, host to OPX and OPX to host |
| `opnic-serial` | the only card | OPNIC card serial |
| `opnic-fake` | | comma-separated bitstrings: a fake card, for tests |
| `result-timeout` | `120` | seconds to wait for one evaluation's results |

## Max-Cut in one call

```cpp
#include <cudaq/algorithms/max_cut.h>

auto r = cudaq::max_cut(5, /*seed=*/1);  // Erdos-Renyi graph, same as networkx
std::printf("%s cuts %d edges\n", r.bitstring.c_str(), r.cut);
```

`cudaq::max_cut_options` sets `layers`, `shots`, `max_eval`, `initial_step`,
`tol`, `density`, and `init_seed`. The defaults match
`experiments-ethernet/qaoa_with_opt.py`. Bitstrings are clbit 0 first.

A graph with more nodes than `qpu_qubits` (default 5) is split into subgraphs
that share boundary nodes, solved one after another, and merged as the Python
ADC-QAOA (`adc_qaoa_ethernet`) does; each subgraph keeps its
`subgraph_answers` (default 1) most frequent bitstrings for the merge.
Subgraphs of the same shape are renumbered into one order first, so they share
one compiled program (checked offline on the fake OPNIC card); the merge itself
has been checked on the simulator only.

## Limits

- OPNIC packets are fixed: at most 16 parameters in, and 5 measured bits per
  shot out (128 shots per packet).
- Only `a * param + b` angles stay parametric. Any other use of a parameter
  bakes in its value and recompiles each time, with a warning.
- The QOP cluster is shared. Every run prints `quantum_machines: opened QM-...`
  and `closed QM-...`. An open machine that is not in your output belongs to
  someone else, so leave it alone.

## Tests

All of these run offline against `tools/mock_qop.py` unless `--live` is given.
`--live` opens a quantum machine and plays pulses on the OPX1000.

```bash
bash tests/run_p5_checks.sh            # QAOA on all transports; OPNIC uses a fake card
bash tests/run_p5_checks.sh --live --maxeval 2
P5_SRC=tests/p5c_live.cpp bash tests/run_p5_checks.sh --live --spike --qm-transport-opnic
P5_SRC=tests/p5c_live.cpp bash tests/run_p5_checks.sh --live --maxeval 10 --qm-transport-opnic
```

`run_p2_checks.sh`, `run_p3_checks.sh`, and `run_p4_checks.sh` cover the gRPC
layer, the executor, and Bell end to end.
