/*******************************************************************************
 * Copyright (c) 2026 NVIDIA Corporation & Affiliates.                         *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// clang-format off
// RUN: nvq++ %s -o %t && %t | FileCheck %s
// clang-format on

// cudaq::max_cut: graphs and initial parameters must match what networkx and
// numpy give the user's Python QAOA scripts for the same seeds (expected values
// below were produced by networkx 3.4.2 and numpy's legacy seeding), and the
// solver must return a well-formed answer on the simulator.

#include <cstdio>
#include <cudaq/algorithms/max_cut.h>

static int bruteForce(int n, const std::vector<std::pair<int, int>> &edges) {
  int best = 0;
  for (int m = 0; m < (1 << n); ++m) {
    std::string bits(n, '0');
    for (int i = 0; i < n; ++i)
      bits[i] = '0' + ((m >> i) & 1);
    best = std::max(best, cudaq::detail::cut_value(bits, edges));
  }
  return best;
}

int main() {
  const std::pair<int, std::uint64_t> graphs[] = {
      {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4}, {5, 5},
      {5, 6}, {5, 7}, {5, 8}, {5, 9}, {8, 3}, {12, 1099511627783ull}};
  for (auto [n, seed] : graphs) {
    std::printf("graph n=%d seed=%llu:", n, (unsigned long long)seed);
    for (auto [a, b] : cudaq::erdos_renyi_graph(n, 0.5, seed))
      std::printf(" (%d,%d)", a, b);
    std::printf("\n");
  }
  for (int dim : {2, 4}) {
    std::printf("x0 dim=%d:", dim);
    for (double x : cudaq::detail::initial_parameters(42, dim))
      std::printf(" %.12f", x);
    std::printf("\n");
  }

  // How close QAOA p=1 gets depends on shot noise and the starting point, so
  // only a well-formed, honestly scored answer is asserted.
  cudaq::set_random_seed(13);
  const auto edges = cudaq::erdos_renyi_graph(5, 0.5, 1);
  const auto r = cudaq::max_cut(5, 1);
  const bool binary = r.bitstring.find_first_not_of("01") == std::string::npos;
  std::printf("max_cut n=5 seed=1: %s\n",
              binary && r.bitstring.size() == 5 &&
                      r.cut == cudaq::detail::cut_value(r.bitstring, edges) &&
                      r.cut <= bruteForce(5, edges)
                  ? "well-formed"
                  : "MALFORMED");
  return 0;
}

// clang-format off
// CHECK: graph n=5 seed=0: (0,3) (0,4) (1,3) (2,3) (2,4)
// CHECK: graph n=5 seed=1: (0,1) (0,4) (1,2) (1,3) (2,4) (3,4)
// CHECK: graph n=5 seed=2: (0,3) (0,4) (2,3)
// CHECK: graph n=5 seed=3: (0,1) (0,3) (1,3) (1,4) (2,4) (3,4)
// CHECK: graph n=5 seed=4: (0,1) (0,2) (0,3) (0,4) (1,2) (1,3) (3,4)
// CHECK: graph n=5 seed=5: (1,4) (2,3)
// CHECK: graph n=5 seed=6: (0,3) (0,4) (1,2) (1,4) (2,4)
// CHECK: graph n=5 seed=7: (0,1) (0,2) (0,4) (1,3) (1,4) (2,4) (3,4)
// CHECK: graph n=5 seed=8: (0,1) (0,3) (1,2) (1,3) (2,3) (3,4)
// CHECK: graph n=5 seed=9: (0,1) (0,2) (0,3) (1,2) (2,3)
// CHECK: graph n=8 seed=3: (0,1) (0,3) (0,6) (0,7) (1,3) (1,4) (1,6) (2,3) (2,5) (3,7) (4,7) (5,6) (6,7)
// CHECK: graph n=12 seed=1099511627783: (0,5) (0,7) (0,8) (0,10) (0,11) (1,3) (1,6) (1,10) (1,11) (2,4) (2,10) (2,11) (3,4) (3,5) (3,10) (4,5) (4,7) (4,9) (4,10) (5,6) (5,7) (5,8) (5,9) (6,8) (6,9) (6,10) (7,8) (7,11) (8,9) (10,11)
// CHECK: x0 dim=2: 2.353304971691 5.973514161360
// CHECK: x0 dim=4: 2.353304971691 5.973514161360 4.599253580134 3.761482191925
// CHECK: max_cut n=5 seed=1: well-formed
// clang-format on
