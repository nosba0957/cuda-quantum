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
// solver must return a well-formed answer on the simulator. Graphs too large
// for the QPU must be split as the Python ADC-QAOA splits them, and merged into
// the best cut when every subgraph keeps all its bitstrings. Subgraphs of the
// same shape must be renumbered into the same edges (so one circuit structure),
// without changing the merged answer.

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

  // Subgraphs of graphs too large for the QPU, as ADC-QAOA's `random_cut`
  // splits driver.py's graph for the same seed (expected values from Python).
  const int splits[][3] = {{12, 5, 4}, {8, 3, 3}, {11, 4, 4}, {20, 5, 4}};
  for (auto [n, q, seed] : splits) {
    int i = 0;
    for (auto &s :
         cudaq::detail::split(n, cudaq::erdos_renyi_graph(n, 0.5, seed), q)) {
      std::printf("split n=%d q=%d seed=%d #%d: nodes %d..%d:", n, q, seed, i++,
                  s.first, s.first + s.size - 1);
      for (auto [a, b] : s.edges)
        std::printf(" (%d,%d)", a, b);
      std::printf("\n");
    }
  }

  // Renumbering: isomorphic subgraphs get the same edges, and an answer over
  // the renumbered nodes maps back to one with the same cut of the subgraph's
  // own edges, for every bitstring.
  for (auto [n, q, seed] : splits) {
    int i = 0;
    for (auto &s :
         cudaq::detail::split(n, cudaq::erdos_renyi_graph(n, 0.5, seed), q)) {
      std::vector<int> order;
      const auto edges = cudaq::detail::canonical_edges(s, order);
      std::printf("canonical n=%d q=%d seed=%d #%d: %d nodes:", n, q, seed, i++,
                  s.size);
      for (auto [a, b] : edges)
        std::printf(" (%d,%d)", a, b);
      bool same = true;
      for (int m = 0; m < (1 << s.size); ++m) {
        std::string bits(s.size, '0');
        for (int j = 0; j < s.size; ++j)
          bits[j] = '0' + ((m >> j) & 1);
        same &= cudaq::detail::cut_value(bits, edges) ==
                cudaq::detail::cut_value(
                    cudaq::detail::original_order(bits, order), s.edges);
      }
      std::printf(" %s\n", same ? "same cuts" : "CUTS DIFFER");
    }
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
  std::printf("max_cut n=5 seed=1 result: %s cut=%d\n", r.bitstring.c_str(),
              r.cut);

  // When every subgraph keeps all its bitstrings (enough shots to see each),
  // the merge joins every assignment of the graph, so it must find the best.
  for (auto [n, q, seed] : splits) {
    if (n > 12)
      continue;
    cudaq::max_cut_options options;
    options.qpu_qubits = q;
    options.subgraph_answers = 1 << n;
    options.shots = 100000;
    const auto edges = cudaq::erdos_renyi_graph(n, 0.5, seed);
    cudaq::max_cut_result m;
    try {
      m = cudaq::max_cut(n, seed, options);
    } catch (const std::runtime_error &e) {
      // (11, 4) splits into a last subgraph larger than the QPU.
      std::printf("split max_cut n=%d q=%d seed=%d: %s\n", n, q, seed,
                  e.what());
      continue;
    }
    std::printf("split max_cut n=%d q=%d seed=%d: %s\n", n, q, seed,
                m.bitstring.size() == std::size_t(n) &&
                        m.cut == cudaq::detail::cut_value(m.bitstring, edges) &&
                        m.cut == bruteForce(n, edges)
                    ? "best cut"
                    : "NOT BEST");
  }

  // The merged answers before renumbering was added, for the same seeds: each
  // subgraph keeps one bitstring, so the merge depends on every boundary node
  // being where it was. (8, 3, 3)'s third subgraph is renumbered. Shot noise
  // lands on renumbered outcomes, so other seeds can differ.
  const int unchanged[][4] = {{8, 3, 3, 1024}, {12, 5, 4, 100000}};
  for (auto [n, q, seed, shots] : unchanged) {
    cudaq::set_random_seed(13);
    cudaq::max_cut_options options;
    options.qpu_qubits = q;
    options.shots = shots;
    const auto m = cudaq::max_cut(n, seed, options);
    std::printf("default max_cut n=%d q=%d seed=%d: %s cut=%d\n", n, q, seed,
                m.bitstring.c_str(), m.cut);
  }
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
// CHECK: split n=12 q=5 seed=4 #0: nodes 0..4: (0,1) (0,2) (0,3) (0,4) (1,2) (1,3) (1,4) (3,4)
// CHECK: split n=12 q=5 seed=4 #1: nodes 4..8: (0,4) (1,2) (1,4) (2,4) (3,4)
// CHECK: split n=12 q=5 seed=4 #2: nodes 8..11: (0,1) (0,3) (2,3)
// CHECK: split n=8 q=3 seed=3 #0: nodes 0..2: (0,1)
// CHECK: split n=8 q=3 seed=3 #1: nodes 2..4: (0,1)
// CHECK: split n=8 q=3 seed=3 #2: nodes 4..6: (1,2)
// CHECK: split n=8 q=3 seed=3 #3: nodes 6..7: (0,1)
// CHECK: split n=11 q=4 seed=4 #0: nodes 0..2: (0,1) (0,2)
// CHECK: split n=11 q=4 seed=4 #1: nodes 2..4: (0,1) (0,2)
// CHECK: split n=11 q=4 seed=4 #2: nodes 4..6: (0,2)
// CHECK: split n=11 q=4 seed=4 #3: nodes 6..10: (0,1) (0,3) (0,4) (1,3) (2,3) (2,4)
// CHECK: split n=20 q=5 seed=4 #0: nodes 0..4: (0,1) (0,2) (0,3) (0,4) (1,2) (1,3)
// CHECK: split n=20 q=5 seed=4 #1: nodes 4..8: (0,1) (0,2) (0,3) (0,4) (2,3) (2,4) (3,4)
// CHECK: split n=20 q=5 seed=4 #2: nodes 8..12: (0,1) (0,4) (1,2) (2,3) (2,4)
// CHECK: split n=20 q=5 seed=4 #3: nodes 12..16: (0,4) (1,3) (1,4) (2,4)
// CHECK: split n=20 q=5 seed=4 #4: nodes 16..19: (1,2) (2,3)
// CHECK: canonical n=12 q=5 seed=4 #0: 5 nodes:{{.*}} same cuts
// CHECK: canonical n=12 q=5 seed=4 #1: 5 nodes:{{.*}} same cuts
// CHECK: canonical n=12 q=5 seed=4 #2: 4 nodes:{{.*}} same cuts
// CHECK: canonical n=8 q=3 seed=3 #0: 3 nodes: (0,1) same cuts
// CHECK: canonical n=8 q=3 seed=3 #1: 3 nodes: (0,1) same cuts
// CHECK: canonical n=8 q=3 seed=3 #2: 3 nodes: (0,1) same cuts
// CHECK: canonical n=8 q=3 seed=3 #3: 2 nodes: (0,1) same cuts
// CHECK: canonical n=11 q=4 seed=4 #0: 3 nodes: (0,1) (0,2) same cuts
// CHECK: canonical n=11 q=4 seed=4 #1: 3 nodes: (0,1) (0,2) same cuts
// CHECK: canonical n=11 q=4 seed=4 #2: 3 nodes: (0,1) same cuts
// CHECK: canonical n=11 q=4 seed=4 #3: 5 nodes:{{.*}} same cuts
// CHECK: canonical n=20 q=5 seed=4 #0: 5 nodes:{{.*}} same cuts
// CHECK: canonical n=20 q=5 seed=4 #1: 5 nodes:{{.*}} same cuts
// CHECK: canonical n=20 q=5 seed=4 #2: 5 nodes:{{.*}} same cuts
// CHECK: canonical n=20 q=5 seed=4 #3: 5 nodes:{{.*}} same cuts
// CHECK: canonical n=20 q=5 seed=4 #4: 4 nodes:{{.*}} same cuts
// CHECK: max_cut n=5 seed=1: well-formed
// CHECK: max_cut n=5 seed=1 result: 01001 cut=6
// CHECK: split max_cut n=12 q=5 seed=4: best cut
// CHECK: split max_cut n=8 q=3 seed=3: best cut
// CHECK: split max_cut n=11 q=4 seed=4: max_cut: a subgraph has 5 nodes, more than qpu_qubits (4)
// CHECK: default max_cut n=8 q=3 seed=3: 01101010 cut=8
// CHECK: default max_cut n=12 q=5 seed=4: 001111010101 cut=19
// clang-format on
