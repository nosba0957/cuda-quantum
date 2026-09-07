/****************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.               *
 * All rights reserved.                                                     *
 *                                                                          *
 * This source code and the accompanying materials are made available under *
 * the terms of the Apache License 2.0 which accompanies this distribution.  *
 ****************************************************************************/

// Decodes the byte stream GetNamedResults returns for a QUA program built by
// tools/qua_build.py into CUDA-Q counts.
//
// The QUA side is `boolean_to_int().buffer(<creg size>).save_all(<creg name>)`,
// so the stream is a flat run of `creg size` integers per shot, element i being
// classical bit i. There is no record framing and no iteration delimiter: one
// `save_all` accumulates across the whole `iterations` loop, so a batch is
// identified only by counting `shots` buffers off the front.

#pragma once

#include "common/SampleResult.h"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

namespace cudaq::qm {

class QuaResultDecoder {
public:
  /// @param elementBytes width of one QUA int in the result stream. QOP 3.6
  /// sends int64 (measured: numpy dtype '<i8'). QUA `int`
  /// is 32 bits; the QOP does not restate the dtype on this RPC, it is carried
  /// out of band by GetJobNamedResultHeader.
  QuaResultDecoder(std::string registerName, std::size_t cregSize,
                   std::size_t shotsPerBatch, std::size_t elementBytes = 8);

  /// Feed raw DataChunk payload. Chunk boundaries are arbitrary and need not
  /// align with shots.
  void append(std::string_view bytes);

  std::size_t pendingShots() const { return shots.size(); }
  std::size_t pendingBatches() const { return shots.size() / shotsPerBatch; }
  std::size_t totalShotsDecoded() const { return decoded; }

  /// Pop one batch (`shotsPerBatch` shots) into `counts`. False, leaving
  /// `counts` untouched, when a whole batch has not arrived yet.
  bool takeBatch(cudaq::CountsDictionary &counts);

  /// Pop every complete shot decoded so far, whatever the batch boundary.
  cudaq::CountsDictionary takeRemaining();

  const std::string &registerName() const { return name; }
  std::size_t bitstringWidth() const { return cregSize; }

private:
  std::string name;
  std::size_t cregSize;
  std::size_t shotsPerBatch;
  std::size_t elementBytes;
  std::size_t decoded = 0;
  std::string carry;
  std::deque<std::string> shots;
};

/// Bit i of `bits` (LSB-first) becomes character i, matching CUDA-Q's
/// convention that a measurement string is indexed by qubit, leftmost first.
std::string bitstringFromMask(unsigned long long bits, std::size_t width);

inline cudaq::sample_result
toSampleResult(const cudaq::CountsDictionary &counts,
               const std::string &registerName) {
  return cudaq::sample_result(cudaq::ExecutionResult(counts, registerName));
}

} // namespace cudaq::qm
