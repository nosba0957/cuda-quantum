# CUDA-Q Architecture Notes: The MLIR Layer

Working notes on the compiler layer — dialects, adding ops and passes, TableGen
wiring, and how Quake gets produced. Companion to `ARCHITECTURE-BACKEND.md`
(simulators and QPUs) and `CLAUDE.md` (build/test/lint commands).

Read from the source tree, not from running the code. Paths accurate as of the
`main` branch at commit `6e15b2a25c`.

## Layout

Everything MLIR lives under `cudaq/`. The layout follows upstream MLIR/CIRCT
conventions; `cmake/modules/AddCUDAQ.cmake` is explicitly derived from CIRCT's
`AddCIRCT.cmake`, so CIRCT familiarity transfers directly.

```
cudaq/include/cudaq/Optimizer/
  Dialect/{Quake,CC,QEC}/       # <D>Dialect.td, <D>Ops.td, <D>Types.td, <D>Interfaces.td
  Transforms/Passes.td          # ~90 optimization passes
  CodeGen/{Passes.td,CodeGen*.td}  # lowering to QIR/QASM/IQM + internal "codegen" dialect
  Builder/Factory.h             # helpers for constructing IR programmatically
  InitAllDialects.h, InitAllPasses.h
cudaq/lib/Optimizer/            # C++ implementations
cudaq/test/                     # FileCheck (lit) tests — .qke files
```

## The four dialects

| Dialect | Namespace | Purpose |
|---|---|---|
| **Quake** | `cudaq::quake` | Quantum ops: alloca, gates, measurement, kernel calls |
| **CC** | `cudaq::cc` | "Classical computation" — control flow, aggregates, pointers alongside quantum ops |
| **QEC** | `cudaq::qec` | Detectors, logical observables |
| **codegen** | — | Internal only: *"Do not use this dialect outside of code generation"* |

## Quake's dual semantics

Quake's defining feature. Reference form (`!quake.ref`, `!quake.veq<n>`) is
memory-like; value form (`!quake.wire`) is SSA-like. `memtoreg` / `regtomem`
convert between them, and most optimizations want the value form.

Wires are **linear** types — used exactly once, and an op consuming a wire must
return a new one. `QuakeTypes.td` marks them with `SSITypeTrait` for a specific
defensive reason worth quoting, because it explains why the codebase resists
letting stock MLIR passes near quantum values:

> If we allow MLIR's canonicalizer and DCE passes to treat values of linear
> type as SSA values, those passes can conspire to eliminate live wires and
> associated quantum operations. Removing arbitrary live code can (will)
> generate incorrect code.

`!quake.control` is the exception — a control qubit is unmodified, so it *is* a
true SSA value and can feed multiple control positions. Convert with
`quake.to_ctrl` / `quake.from_ctrl`.

## Adding an op

Quantum gates rarely need writing from scratch. `QuakeOps.td` provides a
`QuakeOperator` base class supplying the full
`is_adj`/`parameters`/`controls`/`targets`/`negated_qubit_controls` operand
structure, eight `OpBuilder` overloads, the assembly format, and the
memory-effects implementation. New gates are usually one line:

```tablegen
def quake_FooOp : OneTargetOp<"foo">;    // also OneTargetParamOp, TwoTargetOp
```

For a non-gate op, subclass `QuakeOp<mnemonic, traits>` and add
`let hasVerifier = 1;` / `let hasCanonicalizer = 1;`, then implement
`FooOp::verify()` and `FooOp::getCanonicalizationPatterns()` in
`cudaq/lib/Optimizer/Dialect/Quake/QuakeOps.cpp`.

Build wiring is automatic but **name-sensitive**: `add_cudaq_dialect(Quake quake)`
hardcodes `<Dialect>Dialect.td`, `<Dialect>Ops.td`, `<Dialect>Types.td`. It runs
`mlir-tablegen` to emit `.h.inc`/`.cpp.inc` and creates the
`QuakeDialectIncGen`/`QuakeOpsIncGen`/`QuakeTypesIncGen` targets that the dialect
library lists under `DEPENDS`. A brand-new dialect also needs registering in
`InitAllDialects.h` and, to be reachable from Python, a CAPI entry in
`Optimizer/CAPI/Dialects.h`.

## Adding a pass

Declare in `Transforms/Passes.td`:

```tablegen
def MyPass : Pass<"my-pass", "mlir::func::FuncOp"> {
  let summary = "...";
  let description = [{ ... }];
}
```

That file carries a standing reviewer instruction: passes are in **alphabetical
order**, and out-of-order additions get Request Changes.

Implement in `cudaq/lib/Optimizer/Transforms/MyPass.cpp`, add the `.cpp` to that
directory's `CMakeLists.txt`, and it is auto-registered via
`registerOptTransformsPasses()`. The shape (from `WriteAfterWriteElimination.cpp`):

```cpp
#include "PassDetails.h"
namespace cudaq::opt {
#define GEN_PASS_DEF_MYPASS
#include "cudaq/Optimizer/Transforms/Passes.h.inc"
}
#define DEBUG_TYPE "my-pass"
using namespace mlir;

namespace {
class MyPass : public cudaq::opt::impl::MyPassBase<MyPass> {
public:
  using MyPassBase::MyPassBase;
  void runOnOperation() override { ... }
};
}
```

`PassDetails.h` provides `GATE_OPS` / `MEASURE_OPS` / `QUANTUM_OPS` X-macros for
type-switching over every gate without enumerating them.

**Gotcha:** the `*Patterns.inc` files in `Transforms/` (`LoopUnrollPatterns.inc`,
`WriteAfterWriteEliminationPatterns.inc`, …) are **hand-written C++ checked into
git**, not TableGen DRR output. Most MLIR projects generate `.inc` files, so the
instinct to hunt for a `.td` source is wrong here. They share patterns between
passes and assume `using namespace mlir;` is already in scope at the include site.

Named pipelines (`target-prep`, `aggressive-inlining`, …) are assembled in
`Transforms/Pipelines.cpp` and `CodeGen/Pipelines.cpp`, registered from
`InitAllPasses.h`.

## Three producers of Quake

1. **C++ kernels** — `cudaq/lib/Frontend/nvqpp/` walks the Clang AST
   (`ASTBridge.cpp` + `ConvertDecl/ConvertExpr/ConvertStmt/ConvertType.cpp`).
   This is what `cudaq-quake` runs.
2. **Python kernels** — `python/cudaq/kernel/ast_bridge.py` walks the Python AST
   and calls the MLIR Python bindings (`quake.VeqType.get()`,
   `quake.DiscriminateOp(...)`). Entirely separate from the Clang path. Bindings
   come from `python/cudaq/mlir/dialects/QuakeOps.td` — a stub that just
   `include`s the real `.td` — fed to `declare_mlir_dialect_python_bindings` in
   `python/extension/CMakeLists.txt`.
3. **Inside passes** — `Optimizer/Builder/Factory.h` (namespace
   `cudaq::opt::factory`) for type constructors and constant/temporary helpers;
   `Intrinsics.h` for declaring runtime intrinsics.

## Testing IR work

FileCheck tests via lit in `cudaq/test/Transforms/` (221 files). Write `.qke` —
MLIR text, not C++ — so the pass is exercised in isolation:

```mlir
// RUN: cudaq-opt -write-after-write-elimination %s | FileCheck %s
func.func @test() { ... }
// CHECK-LABEL: func.func @test() {
// CHECK:         %[[VAL_1:.*]] = quake.alloca !quake.veq<2>
```

`cudaq/test/AST-Quake/` tests the Clang frontend from `.cpp` inputs instead.
Run with `ninja check-cudaq`, or point `llvm-lit` at a single file.
`Developing.md` expects any compiler-output change to ship with a FileCheck test.

Iterating by hand:

```bash
cudaq-quake foo.cpp | cudaq-opt --canonicalize --add-dealloc | cudaq-translate --convert-to=qir
```

`nvq++` builds its pass pipeline as a *string* in bash (`nvq++.in`), and that
sequence must stay in sync with the `PythonAOT` pipeline in `Pipelines.cpp`.
A comment marks the constraint; nothing enforces it.
