/*******************************************************************************
 * Copyright (c) 2026 NVIDIA Corporation & Affiliates.                         *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// Records which rotation angles depend on the kernel's open variational
// parameters, so a target can compile the circuit structure once and send only
// the parameter values afterwards. See the ParameterTable pass description.

#include "PassDetails.h"
#include "cudaq/Frontend/nvqpp/AttributeNames.h"
#include "cudaq/Optimizer/Transforms/Passes.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include <cstdio>

namespace cudaq::opt {
#define GEN_PASS_DEF_PARAMETERTABLE
#include "cudaq/Optimizer/Transforms/Passes.h.inc"
} // namespace cudaq::opt

#define DEBUG_TYPE "parameter-table"

using namespace mlir;

namespace {
/// `a * params[k] + b`; `k < 0` means the value does not depend on the params.
struct Affine {
  int64_t k = -1;
  double a = 0;
  double b = 0;
};

/// Returns the element index if `v` is `load(compute_ptr(stdvec_data(arg),
/// const))`, looking through pointer casts.
std::optional<int64_t> paramIndex(Value v, BlockArgument arg) {
  auto load = v.getDefiningOp<cudaq::cc::LoadOp>();
  if (!load)
    return std::nullopt;
  Value ptr = load.getPtrvalue();
  while (auto cast = ptr.getDefiningOp<cudaq::cc::CastOp>())
    ptr = cast.getValue();
  // Element 0 is folded to a bare cast of the data pointer.
  if (auto data = ptr.getDefiningOp<cudaq::cc::StdvecDataOp>())
    return data.getStdvec() == arg ? std::optional<int64_t>(0) : std::nullopt;
  auto gep = ptr.getDefiningOp<cudaq::cc::ComputePtrOp>();
  if (!gep || gep.getNumIndices() != 1)
    return std::nullopt;
  Value base = gep.getBase();
  while (auto cast = base.getDefiningOp<cudaq::cc::CastOp>())
    base = cast.getValue();
  auto data = base.getDefiningOp<cudaq::cc::StdvecDataOp>();
  if (!data || data.getStdvec() != arg)
    return std::nullopt;
  if (auto idx = gep.getConstantIndex(0))
    return *idx;
  return std::nullopt;
}

std::optional<Affine> matchAffine(Value v, BlockArgument arg) {
  if (auto c = v.getDefiningOp<arith::ConstantOp>())
    if (auto f = dyn_cast<FloatAttr>(c.getValue()))
      return Affine{-1, 0, f.getValueAsDouble()};
  if (auto k = paramIndex(v, arg))
    return Affine{*k, 1, 0};
  Operation *op = v.getDefiningOp();
  if (!op)
    return std::nullopt;
  if (isa<arith::ExtFOp, arith::TruncFOp, cudaq::cc::CastOp>(op))
    return matchAffine(op->getOperand(0), arg);
  if (auto neg = dyn_cast<arith::NegFOp>(op)) {
    auto x = matchAffine(neg.getOperand(), arg);
    if (x)
      return Affine{x->k, -x->a, -x->b};
    return std::nullopt;
  }
  if (!isa<arith::AddFOp, arith::SubFOp, arith::MulFOp, arith::DivFOp>(op))
    return std::nullopt;
  auto l = matchAffine(op->getOperand(0), arg);
  auto r = matchAffine(op->getOperand(1), arg);
  if (!l || !r)
    return std::nullopt;
  if (isa<arith::SubFOp>(op))
    *r = Affine{r->k, -r->a, -r->b};
  if (isa<arith::AddFOp, arith::SubFOp>(op)) {
    if (l->k >= 0 && r->k >= 0 && l->k != r->k)
      return std::nullopt; // depends on two parameters
    return Affine{std::max(l->k, r->k), l->a + r->a, l->b + r->b};
  }
  if (isa<arith::MulFOp>(op)) {
    if (l->k >= 0 && r->k >= 0)
      return std::nullopt; // product of parameters
    const Affine &x = l->k >= 0 ? *l : *r;
    double c = l->k >= 0 ? r->b : l->b;
    return Affine{x.k, x.a * c, x.b * c};
  }
  if (r->k >= 0) // dividing by a parameter
    return std::nullopt;
  return Affine{l->k, l->a / r->b, l->b / r->b};
}

/// True when every read of `arg` flows only into rotation parameters, through
/// element addressing, casts and float arithmetic.
bool onlyFeedsRotations(BlockArgument arg) {
  SmallVector<Value> work{arg};
  while (!work.empty()) {
    for (OpOperand &use : work.pop_back_val().getUses()) {
      Operation *user = use.getOwner();
      if (auto optor = dyn_cast<cudaq::quake::OperatorInterface>(user)) {
        auto params = optor.getParameters();
        unsigned first = params.empty() ? 0 : params.getBeginOperandIndex();
        if (params.empty() || use.getOperandNumber() < first ||
            use.getOperandNumber() >= first + params.size())
          return false;
        continue;
      }
      if (!isa<cudaq::cc::StdvecDataOp, cudaq::cc::CastOp,
               cudaq::cc::ComputePtrOp, cudaq::cc::LoadOp, arith::AddFOp,
               arith::SubFOp, arith::MulFOp, arith::DivFOp, arith::NegFOp,
               arith::ExtFOp, arith::TruncFOp>(user))
        return false;
      work.append(user->result_begin(), user->result_end());
    }
  }
  return true;
}

std::string formatAffine(const Affine &x) {
  // %.17g round-trips a double; QASM consumers parse these back.
  char buf[96];
  if (x.b != 0)
    std::snprintf(buf, sizeof buf, "%.17g*p%lld+%.17g", x.a, (long long)x.k,
                  x.b);
  else
    std::snprintf(buf, sizeof buf, "%.17g*p%lld", x.a, (long long)x.k);
  return buf;
}

class ParameterTablePass
    : public cudaq::opt::impl::ParameterTableBase<ParameterTablePass> {
public:
  using ParameterTableBase::ParameterTableBase;

  void runOnOperation() override {
    ModuleOp mod = getOperation();
    auto values = mod->getAttrOfType<DenseF64ArrayAttr>(
        cudaq::variationalParametersAttrName);
    if (!values)
      return;
    func::FuncOp entry;
    mod.walk([&](func::FuncOp f) {
      if (f->hasAttr(cudaq::entryPointAttrName))
        entry = f;
    });
    BlockArgument arg;
    if (entry && !entry.isDeclaration())
      for (BlockArgument a : entry.getArguments())
        if (auto vt = dyn_cast<cudaq::cc::StdvecType>(a.getType()))
          if (vt.getElementType().isF64())
            arg = a;
    if (!arg) {
      mod->removeAttr(cudaq::variationalParametersAttrName);
      return;
    }

    // Every rotation angle that reads the parameters must be affine in one of
    // them, and nothing else may read them.
    SmallVector<std::tuple<Operation *, unsigned, Affine>> rewrites;
    bool affine = onlyFeedsRotations(arg);
    entry.walk([&](cudaq::quake::OperatorInterface optor) {
      for (auto [i, p] : llvm::enumerate(optor.getParameters())) {
        if (p.getDefiningOp<arith::ConstantOp>())
          continue;
        auto x = matchAffine(p, arg);
        if (!x || x->k >= (int64_t)values.size()) {
          affine = false;
          continue;
        }
        rewrites.emplace_back(optor.getOperation(), i, *x);
      }
    });

    OpBuilder builder(&getContext());
    auto constant = [&](Operation *op, double v) {
      builder.setInsertionPoint(op);
      return arith::ConstantOp::create(builder, op->getLoc(),
                                       builder.getF64FloatAttr(v));
    };

    if (affine) {
      // Each parametric angle becomes a placeholder constant plus the
      // expression the code emitter prints in its place.
      DenseMap<Operation *, SmallVector<Attribute>> exprs;
      for (auto &[op, i, x] : rewrites) {
        auto optor = cast<cudaq::quake::OperatorInterface>(op);
        auto &e = exprs[op];
        e.resize(optor.getParameters().size(), builder.getStringAttr(""));
        // The placeholder is the angle at params = 0; exact when a == 0.
        if (x.a != 0)
          e[i] = builder.getStringAttr(formatAffine(x));
        op->setOperand(optor.getParameters().getBeginOperandIndex() + i,
                       constant(op, x.b));
      }
      for (auto &[op, e] : exprs)
        op->setAttr(cudaq::parameterExprsAttrName, builder.getArrayAttr(e));
    } else {
      // Bake the actual values in, the same result argument synthesis gives.
      entry.emitWarning("variational parameters feed more than affine "
                        "rotation angles; baking them in, so every evaluation "
                        "is a separate compile");
      SmallVector<cudaq::cc::LoadOp> loads;
      entry.walk([&](cudaq::cc::LoadOp load) {
        if (paramIndex(load, arg))
          loads.push_back(load);
      });
      for (auto load : loads) {
        auto k = *paramIndex(load, arg);
        if (k >= (int64_t)values.size()) {
          load.emitError("variational parameter index out of range");
          return signalPassFailure();
        }
        load.replaceAllUsesWith(constant(load, values[k]).getResult());
      }
      mod->removeAttr(cudaq::variationalParametersAttrName);
    }

    // Fold what the rewrite left constant and drop the now-dead reads.
    if (failed(applyPatternsGreedily(entry, RewritePatternSet(&getContext()))))
      return signalPassFailure();
    if (!arg.use_empty()) {
      Operation *user = *arg.getUsers().begin();
      user->emitError("variational parameters are still read (by ")
          << user->getName() << ") after the parameter table was built";
      signalPassFailure();
    }
  }
};
} // namespace
