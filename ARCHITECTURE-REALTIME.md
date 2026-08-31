# CUDA-Q Architecture Notes: The Realtime Layer (NVQLink)

Working notes on `realtime/` — the backend for integrators wiring a GPU directly
to a quantum controller over a low-latency link. Companion to
`ARCHITECTURE-MLIR.md` (compiler) and `ARCHITECTURE-BACKEND.md` (simulators and
REST QPUs); `CLAUDE.md` has build/test/lint commands.

Read from the source tree, not from running the code. Paths accurate as of the
`main` branch at commit `6e15b2a25c`.

## What it is, and how it differs from the other backends

`ARCHITECTURE-BACKEND.md` covers backends where CUDA-Q *submits a job*: compile
the kernel to QIR, POST it to a vendor REST API, poll for results. Latency there
is seconds to minutes and nobody cares.

Realtime is the opposite regime. The classical computation sits **inside** the
coherence window of the quantum program — a QEC decoder that has to return a
correction before the qubits decohere. The measured target on NVIDIA's own
validation rig, from `docs/user_guide.md`:

```text
=== PTP Round-Trip Latency ===
  Samples:  100
  Min:      3589 ns
  Max:      6348 ns
  Avg:      3872.0 ns
```

Roughly 3.9 µs FPGA → GPU → FPGA. That number is what every design decision in
this subsystem is paying for.

The README states the two responsibilities plainly:

> 1. It provides the low-level basis of realtime coprocessing between FPGA and
>    CPU-GPU systems.
> 2. It provides the low latency networking stack of the NVQLink architecture,
>    enabling system integrators to achieve few-microsecond data round trips
>    between FPGA and GPU.

Note the audience: **system integrators**, not algorithm authors. This is the
backend you build against when you own the control electronics.

## Two halves

The subsystem splits across two trees that are deliberately decoupled:

| | Location | Role |
|---|---|---|
| **Realtime library** | `realtime/` | Standalone. Transport providers + dispatcher. Knows nothing about quantum kernels — it moves RPC frames and calls handlers. |
| **CUDA-Q integration** | `runtime/internal/device_call/` + `cudaq/lib/Optimizer/Transforms/RealtimeDeviceCall.cpp` | Makes `cudaq::device_call(...)` inside a `__qpu__` kernel turn into a frame on that wire. |

`realtime/` has its own `LICENSE`, `NOTICE`, `README.md`, `docs/`, CMake, and CI
(`.github/workflows/realtime_ci.yml`, `realtime_prebuilt_binaries.yml`), and
installs to its own prefix (`/opt/nvidia/cudaq/realtime`). It is a sub-project,
not a subdirectory.

## The path from kernel source to wire

```
cudaq::device_call(decoder, syndrome)        runtime/cudaq/driver/device.h
  → cc.device_call                           CC dialect op (CCOps.td:1873)
  → -frealtime-lowering                      nvq++ flag
  → realtime-device-call pass                RealtimeDeviceCall.cpp
  → __cudaq_device_call_{acquire,dispatch,safely_release}_realtime_frame
  → DeviceCallChannel plugin                 runtime/internal/device_call/
  → bridge provider (dlopen'd)               realtime/lib/daemon/bridge/
  → NIC → FPGA → quantum controller
```

Worth knowing at each step:

**`runtime/cudaq/driver/device.h`** — the `cudaq::device_call` templates are
plain `std::invoke` passthroughs. Reading this header tells you nothing about
realtime; it is the host-side fallback so the same source compiles and runs
locally. All the actual behavior comes from compiler lowering.

**`cc.device_call`** — a CC dialect op, not Quake. Its summary: *"In a tightly
coupled environment, the quantum code may callback to regular classical C++ code
on the control driver."* It carries optional `numBlocks`/`numThreadsPerBlock`
(CUDA launch geometry) and a `device` operand, matching the `device_call`
overloads that take `<BlockSize, GridSize>` template parameters.

**Two lowerings compete for the same op.** `nvq++` picks based on
`-frealtime-lowering` (default off, `nvq++.in:560`):

- `realtime-device-call` → the leased-frame ABI described here.
- `distributed-device-call` → a distributed-memory model, with an
  `insert-trap` option that emits a weak-linkage runtime trap as the default
  implementation so it can be overridden at link time.

The realtime pipeline is `realtime-device-call,inline,canonicalize,cse`
(`nvq++.in:1068`) — `inline` is mandatory, not cosmetic, because the pass emits
calls to private marshaling intrinsics (`__nvqpp_RealtimePackBits`,
`__nvqpp_RealtimePackMeasurements`, `__nvqpp_RealtimeUnpackBits`) that must be
expanded at their call sites.

`nvq++` errors out if `-frealtime-lowering` is passed to a build without
realtime support compiled in.

**Type restrictions.** `RealtimeDeviceCall.cpp` only marshals a fixed set:
scalars are `i1/i8/i16/i32/i64`, `f32`, `f64`; primitive array elements narrow
further to `i1/i8/i32`, `f32`, `f64`. Anything else fails to lower. This is the
wire format leaking into the language surface, and it is the first thing to
check when a `device_call` won't compile.

## The wire protocol

Hardware-agnostic and specified in `docs/cudaq_realtime_message_protocol.md`.
Each ring-buffer slot is:

```text
| RPCHeader | payload bytes (arg_len) | unused padding |
```

Both headers are exactly 24 bytes, packed:

```cpp
struct RPCHeader {
  uint32_t magic;          // 'CUQR' = 0x43555152
  uint32_t function_id;    // fnv1a_hash("handler_name")
  uint32_t arg_len;
  uint32_t request_id;     // caller-assigned, echoed back
  uint64_t ptp_timestamp;  // PTP send timestamp, echoed back
};

struct RPCResponse {
  uint32_t magic;          // 'CUQS' = 0x43555153
  int32_t  status;         // 0 = success
  uint32_t result_len;
  uint32_t request_id;
  uint64_t ptp_timestamp;
};
```

Design points that matter:

- **The payload is typeless on the wire.** Interpretation comes from a schema
  registered in the dispatcher's function table. The spec is explicit that this
  is an *out-of-band contract*: FPGA firmware and function table must agree, and
  *"schema mismatches are detected during integration testing, not at runtime."*
  There is no negotiation and no version check on the payload.
- **`function_id` is an FNV-1a hash of the handler name.** No registry, no
  string on the wire.
- **`ptp_timestamp` is infrastructure, not application data.** The dispatcher
  echoes it verbatim in every path; handlers never touch it. That is what makes
  the latency measurement above possible without instrumenting handlers.
- **`request_id` is opaque and echoed.** Conventionally the shot index or a
  sequence number, enabling pipelined/out-of-order response matching.
- Everything is little-endian, IEEE 754, tightly packed. `TYPE_BIT_PACKED` is
  LSB-first with `size_bytes = ceil(num_elements / 8)` — the natural encoding
  for binary detection events.

The protocol doc's QEC section is the clearest statement of intent: one RPC
message is one **decoding round**, not one shot, and it deliberately introduces
that term to avoid the ambiguity.

## Dispatch architectures

Two orthogonal choices, both in `cudaq_realtime.h`.

**Where the dispatch loop runs** (`cudaq_dispatch_path_t`):

- `CUDAQ_DISPATCH_PATH_DEVICE` — persistent GPU kernel.
- `CUDAQ_DISPATCH_PATH_HOST` — CPU host loop. Supports GPU-less bridges.

**Kernel synchronization shape** (`cudaq_kernel_type_t`): `REGULAR`,
`COOPERATIVE`, `UNIFIED`.

The **3-kernel architecture** is the baseline: separate persistent RX, dispatch,
and TX GPU kernels communicating through ring buffers with `rx_flags`/`tx_flags`.
The docs justify it on separation of concerns, reusability, testability, and
transport independence.

**Unified dispatch mode** (`CUDAQ_KERNEL_UNIFIED`) collapses all three into a
single GPU thread running a transport-provided kernel: poll for message → parse
`RPCHeader` → call handler in place → write `RPCResponse` → send → re-post
receive. It eliminates the inter-kernel flag handoff, which is the dominant
latency cost. The ring layout is symmetric so **the response overwrites the
request in the same slot**; `request_id` and `ptp_timestamp` are saved to
registers before the handler runs. This is the mode `validate.sh --unified`
exercises and the one that produces the ~3.9 µs figure.

Per-handler invocation mode (`cudaq_dispatch_mode_t`) is a third axis:
`DEVICE_CALL` (direct `__device__` call — lowest latency), `GRAPH_LAUNCH`
(CUDA graph on a worker pool), `HOST_CALL` (synchronous inline C++, bypasses the
worker pool, no CUDA graph, used for GPU-less bridges). The header notes
`DEVICE_CALL` entries are silently *dropped* on the host path.

There is also a reserved non-error status,
`CUDAQ_DISPATCH_STATUS_TRIGGER_GRAPH = 0x12A6E5`, by which a device-call handler
tells the scheduler to fire a pre-configured follow-up graph and then
tail-self-relaunch to reset the fire-and-forget budget. Deliberately chosen not
to collide with success (0) or the negative codes handlers return as errors.

## Transport plugin model

`realtime/include/cudaq/realtime/daemon/bridge/bridge_interface.h` defines a
C ABI that providers implement and expose via
`cudaq_realtime_get_bridge_interface`. Providers are `dlopen`'d by library name
and cached per process, so multiple distinct transports coexist in one process.

Shipped providers under `realtime/lib/daemon/bridge/`:

| Provider | Stack |
|---|---|
| `gpu_roce` | DOCA GPUNetIO + RoCE v2, GPUDirect. The production path; needs ConnectX-7/BlueField. Includes `unified_dispatch_kernel.cu`. |
| `cpu_roce` | `libibverbs` RoCE without GPU involvement (`realtime/lib/cpu_transport/`). |
| `udp` | Plain UDP — for testing without RDMA hardware. |

The interface is **explicitly versioned** (`CUDAQ_REALTIME_BRIDGE_INTERFACE_VERSION 2`).
The loader accepts any version in `[1, CURRENT]`; fields past `disconnect` are
read only from v2+ providers, and a v2 provider may NULL out individual
capabilities, which makes the corresponding API return `CUDAQ_ERR_UNSUPPORTED`.
A v1 provider's struct is allowed to simply end early. This is a real
compatibility contract for third-party transports, not a nominal one.

V2 added three capability queries: `get_cpu_dataplane` (the ring plus `rx_poll`
/ `tx_publish` hooks driving the unified host loop), `get_endpoint_info` (a
one-line `key=value` endpoint description, valid *before* `connect()` blocks, so
a server can publish its rendezvous address first), and `get_ring_geometry` (so
dispatcher config is derived from the transport rather than duplicated).

The `rx_poll` contract is worth quoting for anyone writing a provider:

> Contract: MUST NOT block (return CUDAQ_RX_EMPTY instead of waiting) and should
> be cheap, the dispatch loop calls it in a tight spin.

…and it must publish with acquire/release ordering, "not just a bare store".

## The CUDA-Q side: channel plugins

`runtime/internal/device_call/` is a **second, independent plugin layer** —
don't confuse it with the bridge providers. Implementations: `GpuDispatchChannel`,
`HostDispatchChannel`, `CpuRoceChannel`, `UdpChannel`, over a common
`DeviceCallChannel` / `RingSlotChannel` interface with `RpcFrame` doing the
leased-frame bookkeeping.

Channels are `dlopen`'d by convention from
`libcudaq-device-call-channel-<name>.so`, searched along
`CUDAQ_DEVICE_CALL_PLUGIN_PATH` before the runtime's own directory. Tunables:

```
CUDAQ_DEVICE_CALL_CHANNEL       # which channel
CUDAQ_DEVICE_CALL_PLUGIN_PATH   # extra search dirs
CUDAQ_DEVICE_CALL_SLOTS         # ring slot count
CUDAQ_DEVICE_CALL_SLOT_SIZE     # slot stride (bounds max payload)
CUDAQ_DEVICE_CALL_TIMEOUT_MS
```

Note the headers live under `include/cudaq_internal/device_call/` — this is
Level 2 in the `CppAPICodingStyle.md` layering (internal public module API:
shipped, but explicitly not user-supported).

Separately, `runtime/cudaq/realtime/device_call_service.h` defines a *service*
plugin interface (`DeviceCallService` / `DeviceCallServiceSession`, discovered
via `cudaqGetDeviceCallServicePluginInfo`) for artifacts that own their own
dispatch loop and hand CUDA-Q a pre-populated function table.

## Building and enabling

Realtime is off by default and is a listed sub-project:

```cmake
CUDAQ_ALL_PROJECTS = "cudaq;runtime;python;realtime"
```

Two mutually exclusive ways in:

```bash
# build from the source tree
-DCUDAQ_ENABLE_PROJECTS="cudaq;runtime;realtime"

# or link an already-installed realtime package
-DCUDAQ_REALTIME_DIR=/opt/nvidia/cudaq/realtime
```

Setting both is not an error — CMake warns and drops `realtime` from
`CUDAQ_ENABLE_PROJECTS`, preferring the installed package. Either way CUDA is
required; without it the configure fails with an explicit message. The installed
package must define `cudaq::cudaq-realtime` and `cudaq::cudaq-realtime-dispatch`.

Sub-options: `CUDAQ_REALTIME_BUILD_TESTS`, `CUDAQ_REALTIME_BUILD_EXAMPLES`,
`CUDAQ_REALTIME_ENABLE_HSB_TOOLS`.

Runtime prerequisites for the full FPGA path: CUDA 12+, DOCA 3.3.0 with
`doca-sdk-gpunetio`, a ConnectX-7 or BlueField NIC, and an FPGA programmed with
Holoscan Sensor Bridge IP.

## Testing

The test strategy is built around the fact that almost nobody has the hardware.

- `realtime/unittests/` — split by transport: `bridge_interface/gpu_roce`,
  `bridge_interface/udp`, `cpu_transport`. UDP exists so the bridge interface
  can be tested without RDMA.
- `unittests/device_call/` — the CUDA-Q integration:
  - `test_device_call_dispatch` — labeled `gpu_required;realtime`, `RESOURCE_LOCK "gpu"`.
  - `test_host_dispatch_no_gpu` — deliberately carries **no** `gpu_required`
    label. The binary sets `CUDA_VISIBLE_DEVICES=""` in its own `main()` so the
    pure-`HOST_CALL` path is exercised identically on GPU-less CI runners.
  - `test_cpu_roce_device_call` + `cpu_roce_test_daemon` — built as a separate
    executable specifically so CI can *compile-check* it without running it
    (no RDMA NIC on the runner), using `DISCOVERY_MODE PRE_TEST` so ctest never
    executes the binary at build time. The fixture `GTEST_SKIP`s unless the
    loopback topology is supplied via environment variables.
- End-to-end: `validate.sh` against real HSB hardware, reporting verification
  counts and the PTP latency table.

The CMake comments in `unittests/CMakeLists.txt` are unusually candid about
these tradeoffs (including that the shared-memory device_call test is kept out
of CI by design, per issue #4565) and are worth reading before touching that
file.

## Observations

- **Two `dlopen` plugin layers, easy to confuse.** Bridge providers
  (`libcudaq-realtime-bridge-*.so`, selected by `CUDAQ_REALTIME_BRIDGE_LIB` or
  library name) sit in `realtime/`. Channels
  (`libcudaq-device-call-channel-*.so`, selected by `CUDAQ_DEVICE_CALL_CHANNEL`)
  sit in `runtime/internal/device_call/`. Both are `dlopen`-by-name registries;
  they are not the same mechanism and not the same layer.
- **The schema contract is unenforced.** Typeless payloads plus an FNV-1a
  `function_id` plus an out-of-band schema means a firmware/table mismatch is a
  silent misinterpretation, not an error. The spec says so directly.
- **Slot size bounds the payload**: `max_payload = slot_size - 24`. A syndrome
  that outgrows the ring slot has no fragmentation path in this protocol.
- The unified-mode response-overwrites-request trick is invisible in the API but
  constrains any handler that wants to read its request after writing output.
- `GraphLaunchMode::dispatch` in `dispatch_modes.h` is guarded on
  `__CUDA_ARCH__ >= 900` (Hopper+) and its own comment calls it *"a placeholder —
  actual implementation requires the graph_exec to be properly set up in the
  context."* Treat device-side graph launch as less finished than the rest.
