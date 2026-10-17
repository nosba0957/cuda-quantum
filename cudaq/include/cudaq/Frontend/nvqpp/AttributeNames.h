/****************************************************************-*- C++ -*-****
 * Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#pragma once

namespace cudaq {

/// Name of the annotation attribute attached to CUDA-Q kernels
static constexpr const char kernelAnnotation[] = "quantum";

/// Name of the attribute attached to entry point functions.
static constexpr const char entryPointAttrName[] = "cudaq-entrypoint";

/// Name of the attribute attached to CUDA-Q kernels.
static constexpr const char kernelAttrName[] = "cudaq-kernel";

/// Name of the attribute attached to device call functions.
static constexpr const char deviceCallAttrName[] = "cudaq-devicecall";

/// Name of the annotation attribute attached to unitary generator function for
/// user-defined custom operations
static constexpr const char generatorAnnotation[] =
    "user_custom_quantum_operation";

/// Module attribute holding the values of an entry point's open variational
/// parameters (its `std::vector<double>` argument left out of argument
/// synthesis). Consumed by the `parameter-table` pass.
static constexpr const char variationalParametersAttrName[] =
    "cudaq.variational_parameters";

/// Attribute on a quantum operator naming, per parameter operand, the affine
/// expression of the variational parameters it stands for (`2*p0+0.5`), or ""
/// when the operand is an ordinary constant. Printed by code emitters.
static constexpr const char parameterExprsAttrName[] = "cudaq.parameter_exprs";

} // namespace cudaq
