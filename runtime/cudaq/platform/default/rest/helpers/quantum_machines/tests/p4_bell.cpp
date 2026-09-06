/*******************************************************************************
 * Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// The P4 kernel: a Bell pair sampled through the public cudaq::sample API.
// Compiled by nvq++ against --target quantum_machines, so the whole stack
// (platform -> executor -> builder -> transport -> decoder) runs for real.

#include <cudaq.h>

__qpu__ void bell() {
  cudaq::qvector q(2);
  h(q[0]);
  x<cudaq::ctrl>(q[0], q[1]);
  mz(q);
}

int main() {
  cudaq::sample(1024, bell).dump();
  return 0;
}
