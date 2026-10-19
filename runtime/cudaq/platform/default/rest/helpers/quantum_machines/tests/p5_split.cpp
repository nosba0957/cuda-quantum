/*******************************************************************************
 * Copyright (c) 2026 NVIDIA Corporation & Affiliates.                         *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// A split cudaq::max_cut on the OPX, offline only (run_p5_checks.sh): the
// 8-node graph of seed 3 on a 3-qubit QPU splits into four subgraphs of two
// shapes, three alike (one of them numbered differently) and one 2-node one,
// so renumbering must leave two compiles, not three.
//
//   ./a.out [--maxeval N] <--qm-* settings>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cudaq.h>
#include <cudaq/algorithms/max_cut.h>

static int intArg(int argc, char **argv, const char *flag, int fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], flag))
      return std::atoi(argv[i + 1]);
  return fallback;
}

int main(int argc, char **argv) {
  try {
    cudaq::max_cut_options options;
    options.qpu_qubits = 3;
    options.max_eval = intArg(argc, argv, "--maxeval", 4);
    const auto r = cudaq::max_cut(8, 3, options);
    std::printf("split max_cut n=8 q=3 seed=3: bitstring=%s cut=%d\n",
                r.bitstring.c_str(), r.cut);
    if (r.bitstring.size() != 8 || r.cut < 0)
      throw std::runtime_error("max_cut: malformed answer");
    return 0;
  } catch (const std::exception &e) {
    // An escaping exception would skip the executor's cleanup at exit.
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
