/*******************************************************************************
 * Copyright (c) 2026 NVIDIA Corporation & Affiliates.                         *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// OpnicLink over QM's opnic SDK, CPU streams only. The fake swaps the SDK's
// Configurator for an in-process OPX, so the SDK's own stream classes, ring
// buffers and packet encoding are exercised offline exactly as on the card.

#include "OpnicLink.h"

#include <opnic/opnic.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

// Outside namespace cudaq::qm: QM's packet macros name `qm::` unqualified.
namespace {

struct ParamPacket {
  ::qm::Value<double, cudaq::qm::OpnicMaxParams> params;
  QM_DECLARE_PACKET(ParamPacket, params);
};

struct ResultPacket {
  ::qm::Value<int, cudaq::qm::OpnicShotsPerPacket * cudaq::qm::OpnicMaxBits>
      bits;
  QM_DECLARE_PACKET(ResultPacket, bits);
};

// The ring holds a whole evaluation: 1024 packets is 131072 shots.
using ResultStream =
    ::qm::IncomingStream<ResultPacket, 1024, ::qm::StreamType::CPU>;
using ParamStream = ::qm::OutgoingStream<ParamPacket, 4, ::qm::StreamType::CPU>;

} // namespace

namespace cudaq::qm {
namespace {

/// One driver handle, as the card's: it pins buffers once and then answers
/// EBUSY, so every stream needs its own.
class FakeContext : public ::qm::stream::Context {
public:
  bool is_valid() const override { return true; }
  ::qm::internal::GpuAlignedMemory &gpu_memory_allocate(std::size_t) override {
    throw std::logic_error("FakeContext has no GPU memory");
  }
  std::vector<std::vector<PhysicalEntry>>
  pin_buffers(const std::vector<::qm::stream::PinRequest> &) override {
    if (pinned)
      throw std::runtime_error(
          "PIN_BUFFERS ioctl failed: Device or resource busy");
    pinned = true;
    return {};
  }
  void *mmap_bar(std::uint64_t, std::size_t,
                 ::qm::stream::MemoryAccess) override {
    return nullptr;
  }

private:
  bool pinned = false;
};

/// Plays the OPX behind the SDK: waits for the doorbell a ParamStream::send
/// rings, reads the parameter packet out of the slot, and writes result
/// packets into the incoming ring, advancing its producer index.
class FakeOpx : public ::qm::stream::Configurator {
public:
  FakeOpx(const std::string &results, std::string log) : log(std::move(log)) {
    std::stringstream ss(results);
    for (std::string s; std::getline(ss, s, ',');)
      if (!s.empty())
        bitstrings.push_back(s);
    if (bitstrings.empty())
      throw std::runtime_error("quantum_machines: --qm-opnic-fake needs at "
                               "least one bitstring");
  }

  ~FakeOpx() override { halt(); }

  void halt() {
    stop = true;
    if (worker.joinable())
      worker.join();
  }

  void expect(std::size_t, std::size_t p) { packets = p; }

  // Configurator
  ::qm::stream::IncomingStreamParams
  setup_incoming_stream(std::uint16_t, ::qm::StreamType, std::uint32_t *pi,
                        std::uint32_t *buffer, std::size_t size, std::size_t,
                        std::size_t allocation,
                        std::vector<::qm::signature::SignatureEntry>) override {
    resultRing = buffer;
    resultPi = pi;
    resultSlots = size / allocation;
    return {buffer, pi};
  }
  ::qm::stream::OutgoingStreamParams
  setup_outgoing_stream(std::uint16_t, ::qm::StreamType, std::uint32_t *buffer,
                        std::size_t size, std::size_t, std::size_t allocation,
                        std::vector<::qm::signature::SignatureEntry>) override {
    paramRing = buffer;
    paramSlots = size / allocation;
    worker = std::thread([this] { serve(); });
    return {buffer, &doorbell, &status, nullptr, 0, nullptr, 0};
  }
  void teardown_stream(std::uint16_t, ::qm::StreamDirection) override {}
  void sync() override {}

private:
  void serve() {
    std::size_t paramSlot = 0, resultSlot = 0;
    while (!stop) {
      if (!__atomic_load_n(&doorbell, __ATOMIC_ACQUIRE)) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        continue;
      }
      __atomic_store_n(&doorbell, 0, __ATOMIC_RELEASE);
      const auto packet = ParamPacket::deserialize(
          paramRing +
          paramSlot * (ParamPacket::allocation_size / sizeof(std::uint32_t)));
      paramSlot = (paramSlot + 1) % paramSlots;
      if (!log.empty()) {
        std::ofstream out(log, std::ios::app);
        out << "[";
        for (int k = 0; k < OpnicMaxParams; ++k)
          out << (k ? "," : "") << packet.params[k];
        out << "]\n";
      }
      // Like the OPX, every packet is sent; shots past `shots` are padding.
      for (std::size_t p = 0, shot = 0; p < packets; ++p) {
        ResultPacket result;
        for (int s = 0; s < OpnicShotsPerPacket; ++s, ++shot) {
          const auto &bits = bitstrings[shot % bitstrings.size()];
          for (int q = 0; q < OpnicMaxBits; ++q)
            result.bits[s * OpnicMaxBits + q] =
                q < (int)bits.size() && bits[q] == '1';
        }
        result.serialize(resultRing +
                         resultSlot * (ResultPacket::allocation_size /
                                       sizeof(std::uint32_t)));
        resultSlot = (resultSlot + 1) % resultSlots;
        __atomic_store_n(resultPi, *resultPi + 1, __ATOMIC_RELEASE);
      }
    }
  }

  std::vector<std::string> bitstrings;
  std::string log;
  std::atomic<bool> stop{false};
  std::thread worker;
  std::uint32_t *paramRing = nullptr, *resultRing = nullptr,
                *resultPi = nullptr;
  std::size_t paramSlots = 1, resultSlots = 1;
  std::atomic<std::size_t> packets{0};
  std::uint64_t doorbell = 0, status = 0;
};

class SdkLink : public OpnicLink {
public:
  SdkLink(std::uint16_t toOpx, std::uint16_t fromOpx, const std::string &serial,
          const std::string &fakeResults, const std::string &fakeLog) {
    // The driver pins once per handle, so each stream opens its own, as the
    // SDK's serial-only initialize_stream does.
    auto handlers = [&]() -> ::qm::stream::DeviceHandlers {
      if (!fake)
        return ::qm::stream::make_device_handlers(serial);
      return {std::make_shared<FakeContext>(), fake};
    };
    if (!fakeResults.empty())
      fake = std::make_shared<FakeOpx>(fakeResults, fakeLog);
    auto in = handlers(), out = handlers();
    configurator = in.configurator;
    results = ::qm::initialize_stream<ResultStream>(fromOpx, in.configurator,
                                                    in.context);
    params = ::qm::initialize_stream<ParamStream>(toOpx, out.configurator,
                                                  out.context);
    if (!results || !params)
      throw std::runtime_error(
          "quantum_machines: could not initialise the OPNIC streams " +
          std::to_string(toOpx) + " and " + std::to_string(fromOpx));
  }

  ~SdkLink() override {
    // The fake writes into the streams' rings, so it stops before they go.
    if (fake)
      fake->halt();
  }

  void expect(std::size_t shots, std::size_t packets) override {
    if (fake)
      fake->expect(shots, packets);
  }

  void sync() override { configurator->sync(); }

  void send(const std::vector<double> &values) override {
    if (values.size() > OpnicMaxParams)
      throw std::runtime_error(
          "quantum_machines: " + std::to_string(values.size()) +
          " parameters, but the OPNIC packet carries at most " +
          std::to_string(OpnicMaxParams));
    ParamPacket packet;
    for (int k = 0; k < OpnicMaxParams; ++k)
      packet.params[k] = k < (int)values.size() ? values[k] : 0.0;
    params->send(packet);
  }

  std::vector<std::int32_t> receive(std::size_t packets) override {
    results->wait_for_packets(packets);
    std::vector<std::int32_t> bits;
    bits.reserve(packets * OpnicShotsPerPacket * OpnicMaxBits);
    for (std::size_t p = 0; p < packets; ++p) {
      ResultPacket packet;
      if (results->get(p, packet) != ::qm::status::Success)
        throw std::runtime_error("quantum_machines: OPNIC result packet " +
                                 std::to_string(p) + " missing");
      for (int i = 0; i < OpnicShotsPerPacket * OpnicMaxBits; ++i)
        bits.push_back(packet.bits[i]);
    }
    return bits;
  }

private:
  // Declared first, destroyed last: the streams' deleters still use it.
  std::shared_ptr<FakeOpx> fake;
  std::shared_ptr<::qm::stream::Configurator> configurator;
  ::qm::stream_ptr<ResultStream> results;
  ::qm::stream_ptr<ParamStream> params;
};

} // namespace
} // namespace cudaq::qm

/// The plugin's entry point; QMExecutor's OpnicLink::open loads it with dlopen.
extern "C" cudaq::qm::OpnicLink *cudaq_qm_opnic_open(std::uint16_t toOpx,
                                                     std::uint16_t fromOpx,
                                                     const char *serial,
                                                     const char *fakeResults,
                                                     const char *fakeLog) {
  return new cudaq::qm::SdkLink(toOpx, fromOpx, serial, fakeResults, fakeLog);
}
