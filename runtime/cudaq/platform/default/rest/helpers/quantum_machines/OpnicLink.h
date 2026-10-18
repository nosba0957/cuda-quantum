/****************************************************************-*- C++ -*-****
 * Copyright (c) 2026 NVIDIA Corporation & Affiliates.                         *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// The OPNIC transport: variational parameters to the OPX and measurement bits
// back over QM's OPNIC card, through QM's opnic SDK. See GLOSSARY.md ("OPNIC
// transport") and docs/adr/0001-parameter-transport.md.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cudaq::qm {

/// Packet shapes, fixed at compile time on both sides: qua_build.py declares
/// the same QUA structs and echoes these numbers in its manifest. The result
/// packet is the user's proven 128 shots x 5 bits, which already fills a
/// 4 KB slot, the OPNIC packet limit.
constexpr int OpnicMaxParams = 16;
constexpr int OpnicShotsPerPacket = 128;
constexpr int OpnicMaxBits = 5;

/// Both OPNIC streams of this process. OPNIC streams cannot be initialised
/// twice in one process, so one link serves every program the executor runs.
class OpnicLink {
public:
  virtual ~OpnicLink() = default;

  /// The real card, or with `fakeResults` (comma-separated bitstrings, clbit 0
  /// first) an in-process stand-in for the OPX that answers every parameter
  /// packet with those bitstrings and appends what it received to `fakeLog`.
  static std::unique_ptr<OpnicLink>
  open(std::uint16_t toOpx, std::uint16_t fromOpx, const std::string &serial,
       const std::string &fakeResults, const std::string &fakeLog);

  /// Tells a fake how many shots and packets the running program answers
  /// with. The real OPX knows from its program, so the card ignores this.
  virtual void expect(std::size_t shots, std::size_t packets) {}

  /// Sends one evaluation's parameters, zero-padded to OpnicMaxParams.
  virtual void send(const std::vector<double> &params) = 0;

  /// Blocks for one evaluation's `packets` result packets. Returns their bits
  /// shot-major with stride OpnicMaxBits.
  virtual std::vector<std::int32_t> receive(std::size_t packets) = 0;
};

} // namespace cudaq::qm
