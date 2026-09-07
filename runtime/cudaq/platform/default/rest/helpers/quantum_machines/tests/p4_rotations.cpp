/*******************************************************************************
 * Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// Rotations with angles fixed in the source. They are baked into the pulses,
// so the program carries no input stream and needs nothing pushed at run time.
// Sampled twice at different angles to prove the structure cache keys on the
// angle rather than reusing the first program.

#include <cstdio>
#include <cudaq.h>

__qpu__ void rotated(double theta) {
  cudaq::qvector q(2);
  ry(theta, q[0]);
  rz(0.3, q[0]);
  rx(0.4, q[1]);
  x<cudaq::ctrl>(q[0], q[1]);
  mz(q);
}

int main() {
  for (double theta : {0.5, 2.4}) {
    std::printf("theta=%.1f\n", theta);
    cudaq::sample(256, rotated, theta).dump();
  }
  return 0;
}
