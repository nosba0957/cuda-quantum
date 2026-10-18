/*******************************************************************************
 * Copyright (c) 2026 NVIDIA Corporation & Affiliates.                         *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// The P5c live programs, kept as short as the OPX allows.
//
//   ./a.out --spike <--qm-* settings>       two circuit structures, one
//       evaluation of one 128-shot packet each, on streams initialised once
//   ./a.out [--maxeval N] [--seed S] <--qm-* settings>   cudaq::max_cut(5, S)
//
// Exits non-zero on a wrong shot count or a malformed answer.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cudaq.h>
#include <cudaq/algorithms/max_cut.h>

__qpu__ void oneQubit(std::vector<double> theta) {
  cudaq::qvector q(1);
  rx(theta[0], q[0]);
  mz(q);
}

__qpu__ void twoQubits(std::vector<double> theta) {
  cudaq::qvector q(2);
  h(q[0]);
  x<cudaq::ctrl>(q[0], q[1]);
  rz(theta[0], q[1]);
  mz(q);
}

static int intArg(int argc, char **argv, const char *flag, int fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], flag))
      return std::atoi(argv[i + 1]);
  return fallback;
}

static bool hasFlag(int argc, char **argv, const char *flag) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], flag))
      return true;
  return false;
}

static int run(int argc, char **argv) {
  if (hasFlag(argc, argv, "--spike")) {
    const int shots = 128;
    auto check = [&](const char *name, const cudaq::sample_result &counts) {
      std::size_t total = 0;
      for (auto &[bits, count] : counts)
        total += count;
      std::printf("spike %s: %zu shots\n", name, total);
      counts.dump();
      std::fflush(stdout);
      if (total != shots)
        throw std::runtime_error(std::string("spike: ") + name + " got " +
                                 std::to_string(total) + " shots");
    };
    check("one qubit", cudaq::sample(shots, oneQubit, std::vector{1.0}));
    check("two qubits", cudaq::sample(shots, twoQubits, std::vector{0.5}));
    std::printf("spike passed: two structures on streams initialised once\n");
    return 0;
  }

  cudaq::max_cut_options options;
  options.max_eval = intArg(argc, argv, "--maxeval", 100);
  const int seed = intArg(argc, argv, "--seed", 1);
  const auto r = cudaq::max_cut(5, seed, options);
  std::printf("max_cut n=5 seed=%d: bitstring=%s cut=%d\n", seed,
              r.bitstring.c_str(), r.cut);
  if (r.bitstring.size() != 5 || r.cut < 0)
    throw std::runtime_error("max_cut: malformed answer");
  return 0;
}

// An escaping exception would skip the executor's cleanup at exit, leaving a
// job waiting on the OPX and the quantum machine open.
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
