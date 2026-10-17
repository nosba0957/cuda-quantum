/****************************************************************-*- C++ -*-****
 * Copyright (c) 2026 NVIDIA Corporation & Affiliates.                         *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// Max-Cut by QAOA in one call: draw an Erdos-Renyi graph exactly as networkx
// would for the same seed, optimize a QAOA circuit for it with COBYLA on the
// host, and return the most frequent bitstring at the best parameters.

#pragma once

#include "cudaq.h"
#include "cudaq/optimizers.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace cudaq {

/// Settings for `max_cut`. The defaults are those of the OPX QAOA scripts
/// this was written to reproduce (`experiments-ethernet/qaoa_with_opt.py`).
struct max_cut_options {
  /// QAOA layers p; the circuit has 2p parameters.
  int layers = 1;
  std::size_t shots = 1024;
  /// Cost evaluations COBYLA may spend.
  int max_eval = 100;
  /// COBYLA's initial step (`rhobeg`).
  double initial_step = 0.5;
  /// Relative tolerance on the cost at which COBYLA stops.
  double tol = 1e-5;
  /// Edge probability of the Erdos-Renyi graph.
  double density = 0.5;
  /// Seed of the initial parameters, drawn uniformly from [0, 2*pi) the way
  /// `np.random.seed(s); np.random.uniform(0, 2*np.pi, 2*p)` draws them.
  std::uint32_t init_seed = 42;
};

/// The answer to a Max-Cut instance: `bitstring[i]` is the side of vertex i.
struct max_cut_result {
  std::string bitstring;
  int cut = 0;
};

namespace detail {

/// Python's `random.Random(seed)`: MT19937 seeded by `init_by_array` over the
/// seed's 32-bit words, `random()` built from two draws. numpy's legacy
/// `seed(int)` is `std::mt19937`'s own seeding instead, so the two differ.
class python_random {
  std::uint32_t mt[624];
  int index = 624;

public:
  explicit python_random(std::uint64_t seed) {
    std::vector<std::uint32_t> key;
    do {
      key.push_back(static_cast<std::uint32_t>(seed));
      seed >>= 32;
    } while (seed);
    mt[0] = 19650218u;
    for (int i = 1; i < 624; ++i)
      mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i;
    int i = 1;
    std::size_t j = 0;
    for (std::size_t k = std::max<std::size_t>(624, key.size()); k; --k) {
      mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525u)) + key[j] +
              static_cast<std::uint32_t>(j);
      if (++i >= 624) {
        mt[0] = mt[623];
        i = 1;
      }
      if (++j >= key.size())
        j = 0;
    }
    for (int k = 623; k; --k) {
      mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941u)) - i;
      if (++i >= 624) {
        mt[0] = mt[623];
        i = 1;
      }
    }
    mt[0] = 0x80000000u;
  }

  std::uint32_t next() {
    if (index >= 624) {
      for (int k = 0; k < 624; ++k) {
        std::uint32_t y =
            (mt[k] & 0x80000000u) | (mt[(k + 1) % 624] & 0x7fffffffu);
        mt[k] = mt[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
      }
      index = 0;
    }
    std::uint32_t y = mt[index++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    return y ^ (y >> 18);
  }

  /// A double in [0, 1) with 53 random bits, as CPython and numpy build it.
  double random() {
    const std::uint32_t a = next() >> 5, b = next() >> 6;
    return (a * 67108864.0 + b) / 9007199254740992.0;
  }
};

/// QAOA for Max-Cut: H on every qubit, then per layer `exp(-i gamma Z_u Z_v)`
/// on every edge and an `rx(2 beta)` mixer. `theta` is the gammas then the
/// betas, and is the only argument the parameter-table pass leaves open.
struct max_cut_qaoa {
  void operator()(std::vector<double> theta, std::vector<int> u,
                  std::vector<int> v, int n, int layers) __qpu__ {
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
};

/// `np.random.seed(seed); np.random.uniform(0, 2*np.pi, dim)`: numpy's legacy
/// integer seeding is std::mt19937's, and its doubles take 53 bits.
inline std::vector<double> initial_parameters(std::uint32_t seed, int dim) {
  std::mt19937 rng(seed);
  std::vector<double> x(dim);
  for (auto &v : x) {
    const std::uint32_t a = rng() >> 5, b = rng() >> 6;
    v = 2 * M_PI * ((a * 67108864.0 + b) / 9007199254740992.0);
  }
  return x;
}

inline int cut_value(const std::string &bits,
                     const std::vector<std::pair<int, int>> &edges) {
  int cut = 0;
  for (auto [a, b] : edges)
    cut += bits[a] != bits[b];
  return cut;
}

} // namespace detail

/// The edges of networkx's `erdos_renyi_graph(n, p, seed=seed)`, in its order:
/// one draw per vertex pair `(i, j)`, `i < j`, kept when the draw is below p.
inline std::vector<std::pair<int, int>>
erdos_renyi_graph(std::size_t n, double p, std::uint64_t seed) {
  std::vector<std::pair<int, int>> edges;
  if (p <= 0)
    return edges;
  detail::python_random rng(seed);
  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t j = i + 1; j < n; ++j)
      if (rng.random() < p)
        edges.emplace_back(i, j);
  return edges;
}

/// Solves Max-Cut on `erdos_renyi_graph(n, options.density, seed)` with QAOA.
/// Runs on whatever target the program was compiled for.
inline max_cut_result max_cut(std::size_t n, std::uint64_t seed,
                              const max_cut_options &options = {}) {
  const auto edges = erdos_renyi_graph(n, options.density, seed);
  std::vector<int> u, v;
  for (auto [a, b] : edges) {
    u.push_back(a);
    v.push_back(b);
  }
  const int dim = 2 * options.layers;

  auto expectedCut = [&](const sample_result &counts) {
    double cut = 0;
    std::size_t total = 0;
    for (auto &[bits, count] : counts) {
      cut += count * detail::cut_value(bits, edges);
      total += count;
    }
    return total ? cut / total : 0.0;
  };

  optimizers::cobyla optimizer;
  optimizer.initial_parameters =
      detail::initial_parameters(options.init_seed, dim);
  optimizer.max_eval = options.max_eval;
  optimizer.f_tol = options.tol;
  optimizer.step_size = options.initial_step;
  // Unbounded, as scipy's COBYLA is; NLopt's default box is [-pi, pi].
  optimizer.lower_bounds =
      std::vector<double>(dim, -std::numeric_limits<double>::infinity());
  optimizer.upper_bounds =
      std::vector<double>(dim, std::numeric_limits<double>::infinity());

  auto [best, params] = optimizer.optimize(
      dim, [&](const std::vector<double> &theta, std::vector<double> &) {
        return -expectedCut(cudaq::sample(options.shots, detail::max_cut_qaoa{},
                                          theta, u, v, static_cast<int>(n),
                                          options.layers));
      });

  const auto counts =
      cudaq::sample(options.shots, detail::max_cut_qaoa{}, params, u, v,
                    static_cast<int>(n), options.layers);
  max_cut_result result;
  std::size_t top = 0;
  for (auto &[bits, count] : counts)
    if (count > top || (count == top && bits < result.bitstring)) {
      top = count;
      result.bitstring = bits;
    }
  result.cut = detail::cut_value(result.bitstring, edges);
  return result;
}

} // namespace cudaq
