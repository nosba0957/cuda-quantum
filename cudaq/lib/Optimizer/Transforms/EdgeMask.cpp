/*******************************************************************************
 * Copyright (c) 2026 NVIDIA Corporation & Affiliates.                         *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// The edge-mask pass: turns the ZZ layers of a QAOA circuit into every vertex
// pair's ZZ, each guarded by an open 0/1 parameter, so that one compiled
// program serves every graph on the same number of qubits.

#include "PassDetails.h"
#include "cudaq/Frontend/nvqpp/AttributeNames.h"
#include "cudaq/Optimizer/Dialect/Quake/QuakeOps.h"
#include "cudaq/Optimizer/Transforms/Passes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/IRMapping.h"
#include <set>

namespace cudaq::opt {
#define GEN_PASS_DEF_EDGEMASK
#include "cudaq/Optimizer/Transforms/Passes.h.inc"
} // namespace cudaq::opt

#define DEBUG_TYPE "edge-mask"

using namespace mlir;
namespace quake = cudaq::quake;

namespace {
/// `x [a] b; rz(t) b; x [a] b`, which is exp(-i t/2 Z_a Z_b).
struct ZZ {
  Operation *ops[3];
  int64_t a, b;
};

class EdgeMaskPass : public cudaq::opt::impl::EdgeMaskBase<EdgeMaskPass> {
public:
  using EdgeMaskBase::EdgeMaskBase;

  void runOnOperation() override {
    ModuleOp mod = getOperation();
    func::FuncOp entry;
    mod.walk([&](func::FuncOp f) {
      if (f->hasAttr(cudaq::entryPointAttrName))
        entry = f;
    });
    if (!entry || entry.isDeclaration() || !entry.getBody().hasOneBlock())
      return;
    Block &body = entry.front();

    // One sized register, every qubit taken from it at a constant index.
    quake::AllocaOp alloca;
    for (auto a : body.getOps<quake::AllocaOp>()) {
      auto veq = dyn_cast<quake::VeqType>(a.getType());
      if (alloca || !veq || !veq.hasSpecifiedSize())
        return;
      alloca = a;
    }
    if (!alloca)
      return;
    const int64_t n = cast<quake::VeqType>(alloca.getType()).getSize();
    auto index = [&](Value v) -> int64_t {
      auto ext = v.getDefiningOp<quake::ExtractRefOp>();
      if (!ext || ext.getVeq() != alloca.getResult() || !ext.hasConstantIndex())
        return -1;
      return ext.getConstantIndex();
    };

    // The quantum operations in order, and the runs of back-to-back ZZs.
    SmallVector<Operation *> qops;
    for (Operation &op : body)
      if (isa<quake::QuakeDialect>(op.getDialect()) &&
          !isa<quake::AllocaOp, quake::ExtractRefOp, quake::DeallocOp>(op))
        qops.push_back(&op);
    auto matchZZ = [&](std::size_t i) -> std::optional<ZZ> {
      if (i + 2 >= qops.size())
        return std::nullopt;
      auto x1 = dyn_cast<quake::XOp>(qops[i]);
      auto rz = dyn_cast<quake::RzOp>(qops[i + 1]);
      auto x2 = dyn_cast<quake::XOp>(qops[i + 2]);
      if (!x1 || !rz || !x2 || x1.getControls().size() != 1 ||
          x2.getControls().size() != 1 || !rz.getControls().empty() ||
          rz.isAdj() || x1.getNegatedQubitControls() ||
          x2.getNegatedQubitControls())
        return std::nullopt;
      Value a = x1.getControls()[0], b = x1.getTargets()[0];
      if (x2.getControls()[0] != a || x2.getTargets()[0] != b ||
          rz.getTargets()[0] != b)
        return std::nullopt;
      ZZ zz{{x1, rz, x2}, index(a), index(b)};
      if (zz.a < 0 || zz.b < 0)
        return std::nullopt;
      return zz;
    };
    SmallVector<SmallVector<ZZ>> runs;
    for (std::size_t i = 0; i < qops.size();) {
      auto zz = matchZZ(i);
      if (!zz) {
        ++i;
        continue;
      }
      if (i == 0 || runs.empty() || runs.back().back().ops[2] != qops[i - 1])
        runs.emplace_back();
      runs.back().push_back(*zz);
      i += 3;
    }
    if (runs.empty())
      return;

    // Every run must be one angle over the same set of distinct pairs: then
    // its ZZs commute (all diagonal) and one mask serves every layer.
    auto angleOf = [](const ZZ &zz) {
      auto rz = cast<quake::RzOp>(zz.ops[1]);
      auto c = rz.getParameters()[0].getDefiningOp<arith::ConstantOp>();
      return std::make_pair(c ? c.getValue() : Attribute{},
                            rz->getAttr(cudaq::parameterExprsAttrName));
    };
    auto pairsOf = [](ArrayRef<ZZ> run) {
      std::set<std::pair<int64_t, int64_t>> pairs;
      for (auto &zz : run)
        pairs.emplace(std::min(zz.a, zz.b), std::max(zz.a, zz.b));
      return pairs;
    };
    const auto edges = pairsOf(runs.front());
    for (auto &run : runs) {
      if (run.size() != edges.size() || pairsOf(run) != edges)
        return;
      for (auto &zz : run)
        if (!angleOf(zz).first || angleOf(zz) != angleOf(run.front()))
          return;
    }

    // The mask goes after the variational parameters: slot k of pair (i, j),
    // i < j in lexicographic order, reads parameter `first + k`.
    SmallVector<double> values;
    if (auto v = mod->getAttrOfType<DenseF64ArrayAttr>(
            cudaq::variationalParametersAttrName))
      values.assign(v.asArrayRef().begin(), v.asArrayRef().end());
    const int64_t first = values.size();
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = i + 1; j < n; ++j)
        values.push_back(edges.count({i, j}) ? 1.0 : 0.0);
    mod->setAttr(cudaq::variationalParametersAttrName,
                 DenseF64ArrayAttr::get(&getContext(), values));

    OpBuilder builder(&getContext());
    SmallVector<Value> qubit(n);
    for (auto ext : body.getOps<quake::ExtractRefOp>())
      if (auto i = index(ext); i >= 0 && !qubit[i])
        qubit[i] = ext;
    builder.setInsertionPointAfter(alloca);
    for (int64_t i = 0; i < n; ++i)
      if (!qubit[i])
        qubit[i] = quake::ExtractRefOp::create(builder, alloca.getLoc(), alloca,
                                               (std::size_t)i);

    for (auto &run : runs) {
      // The last ZZ's angle is defined before every insertion point.
      const ZZ &tmpl = run.back();
      Value a = cast<quake::XOp>(tmpl.ops[0]).getControls()[0];
      Value b = cast<quake::XOp>(tmpl.ops[0]).getTargets()[0];
      builder.setInsertionPointAfter(tmpl.ops[2]);
      int64_t k = first;
      for (int64_t i = 0; i < n; ++i)
        for (int64_t j = i + 1; j < n; ++j, ++k) {
          IRMapping map;
          map.map(a, qubit[i]);
          map.map(b, qubit[j]);
          for (Operation *op : tmpl.ops)
            builder.clone(*op, map)->setAttr(cudaq::conditionParameterAttrName,
                                             builder.getI64IntegerAttr(k));
        }
      for (auto &zz : run)
        for (Operation *op : zz.ops)
          op->erase();
    }
  }
};
} // namespace
