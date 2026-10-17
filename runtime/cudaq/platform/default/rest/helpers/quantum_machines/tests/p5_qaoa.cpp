/*******************************************************************************
 * Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// The P5 program: Max-Cut QAOA on a 5-node ring, optimized by COBYLA on the
// host. Each cost evaluation is one cudaq::sample on the OPX; the expected cut
// is computed here from the returned counts, the way
// experiments-ethernet/qaoa_with_opt.py does it. The ring's closing edge (4,0)
// is not a coupler on the linear 5q4c chip, so the transpiler must route it.
//
//   ./a.out [--maxeval N] [--layers P] <--qm-* settings>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cudaq.h>
#include <cudaq/optimizers.h>

__qpu__ void qaoa(std::vector<double> theta, std::vector<int> u,
                  std::vector<int> v, int n, int layers) {
  cudaq::qvector q(n);
  h(q);
  for (int l = 0; l < layers; ++l) {
    for (std::size_t e = 0; e < u.size(); ++e) {
      x<cudaq::ctrl>(q[u[e]], q[v[e]]);
      rz(2.0 * theta[l], q[v[e]]);
      x<cudaq::ctrl>(q[u[e]], q[v[e]]);
    }
    for (int j = 0; j < n; ++j)
      rx(2.0 * theta[layers + l], q[j]);
  }
  mz(q);
}

static int intArg(int argc, char **argv, const char *flag, int fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], flag))
      return std::atoi(argv[i + 1]);
  return fallback;
}

int main(int argc, char **argv) {
  const int n = 5, shots = 1024;
  const int layers = intArg(argc, argv, "--layers", 1);
  const std::vector<int> u = {0, 1, 2, 3, 4}, v = {1, 2, 3, 4, 0};

  auto expectedCut = [&](const cudaq::sample_result &counts) {
    double cut = 0;
    std::size_t total = 0;
    for (auto &[bits, count] : counts) {
      for (std::size_t e = 0; e < u.size(); ++e)
        cut += count * (bits[u[e]] != bits[v[e]]);
      total += count;
    }
    if (total != shots)
      throw std::runtime_error("p5: got " + std::to_string(total) +
                               " shots, asked for " + std::to_string(shots));
    return cut / total;
  };

  cudaq::optimizers::cobyla optimizer;
  optimizer.max_eval = intArg(argc, argv, "--maxeval", 30);
  optimizer.initial_parameters = std::vector<double>(2 * layers, 0.4);

  int evals = 0;
  auto [best, params] = optimizer.optimize(
      2 * layers, [&](const std::vector<double> &theta, std::vector<double> &) {
        const double cut =
            expectedCut(cudaq::sample(shots, qaoa, theta, u, v, n, layers));
        std::printf("eval %d cut=%.4f theta=", ++evals, cut);
        for (double t : theta)
          std::printf(" %.4f", t);
        std::printf("\n");
        std::fflush(stdout);
        return -cut;
      });

  std::printf("best cut=%.4f after %d evals (max cut of the ring is 4)\n",
              -best, evals);
  cudaq::sample(shots, qaoa, params, u, v, n, layers).dump();
  return 0;
}
