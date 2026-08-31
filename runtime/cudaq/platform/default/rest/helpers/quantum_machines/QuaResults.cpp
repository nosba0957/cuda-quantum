/****************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.               *
 * All rights reserved.                                                     *
 *                                                                          *
 * This source code and the accompanying materials are made available under *
 * the terms of the Apache License 2.0 which accompanies this distribution.  *
 ****************************************************************************/

// Implementation of the GetNamedResults decoder. See QuaResults.h.

#include "QuaResults.h"

#include <stdexcept>

namespace cudaq::qm {

namespace {

long long readElement(const unsigned char *p, std::size_t width) {
  unsigned long long raw = 0;
  for (std::size_t i = 0; i < width; ++i)
    raw |= static_cast<unsigned long long>(p[i]) << (8 * i);
  return static_cast<long long>(raw);
}

} // namespace

std::string bitstringFromMask(unsigned long long bits, std::size_t width) {
  std::string out(width, '0');
  for (std::size_t i = 0; i < width; ++i)
    out[i] = static_cast<char>('0' + ((bits >> i) & 1ULL));
  return out;
}

QuaResultDecoder::QuaResultDecoder(std::string registerName,
                                   std::size_t cregSize,
                                   std::size_t shotsPerBatch,
                                   std::size_t elementBytes)
    : name(std::move(registerName)), cregSize(cregSize),
      shotsPerBatch(shotsPerBatch), elementBytes(elementBytes) {
  if (cregSize == 0 || shotsPerBatch == 0 || elementBytes == 0 ||
      elementBytes > 8)
    throw std::invalid_argument("QuaResultDecoder: bad geometry");
}

void QuaResultDecoder::append(std::string_view bytes) {
  carry.append(bytes);
  const std::size_t shotBytes = cregSize * elementBytes;
  std::size_t offset = 0;
  while (carry.size() - offset >= shotBytes) {
    const auto *p =
        reinterpret_cast<const unsigned char *>(carry.data()) + offset;
    std::string bits(cregSize, '0');
    for (std::size_t i = 0; i < cregSize; ++i)
      bits[i] = readElement(p + i * elementBytes, elementBytes) ? '1' : '0';
    shots.push_back(std::move(bits));
    ++decoded;
    offset += shotBytes;
  }
  carry.erase(0, offset);
}

bool QuaResultDecoder::takeBatch(cudaq::CountsDictionary &counts) {
  if (shots.size() < shotsPerBatch)
    return false;
  for (std::size_t i = 0; i < shotsPerBatch; ++i) {
    ++counts[shots.front()];
    shots.pop_front();
  }
  return true;
}

cudaq::CountsDictionary QuaResultDecoder::takeRemaining() {
  cudaq::CountsDictionary counts;
  while (!shots.empty()) {
    ++counts[shots.front()];
    shots.pop_front();
  }
  return counts;
}

} // namespace cudaq::qm
