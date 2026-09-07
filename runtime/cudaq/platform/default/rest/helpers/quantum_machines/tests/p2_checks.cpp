/****************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.               *
 * All rights reserved.                                                     *
 *                                                                          *
 * This source code and the accompanying materials are made available under *
 * the terms of the Apache License 2.0 which accompanies this distribution.  *
 ****************************************************************************/

// Runnable checks for the P2 pieces: the gRPC frame codec, the GetNamedResults
// decoder, and -- against tools/mock_qop.py -- the six RPCs plus the one
// transport property the design rests on, that libcurl hands streamed frames to
// the write callback as they arrive instead of buffering the response.
//
// Build and run via tests/run_p2_checks.sh.

#include "GrpcCurl.h"
#include "QuaResults.h"
#include "qm_min.pb.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace cudaq::qm;
using Clock = std::chrono::steady_clock;

namespace {

std::string readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string option(int argc, char **argv, const std::string &flag,
                   const std::string &fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (flag == argv[i])
      return argv[i + 1];
  return fallback;
}

bool hasFlag(int argc, char **argv, const std::string &flag) {
  for (int i = 1; i < argc; ++i)
    if (flag == argv[i])
      return true;
  return false;
}

void checkFrameCodec() {
  const std::string payload = "hello gRPC";
  const std::string framed = encodeFrame(payload);
  assert(framed.size() == payload.size() + 5);
  assert(framed[0] == 0);
  assert(static_cast<unsigned char>(framed[4]) == payload.size());

  // Three frames arriving one byte at a time still decode as three frames.
  std::string wire =
      encodeFrame("a") + encodeFrame("") + encodeFrame(std::string(300, 'z'));
  std::vector<std::string> got;
  GrpcFrameReader reader;
  auto sink = [&](std::string_view f) { got.emplace_back(f); };
  for (char c : wire)
    reader.feed(std::string_view(&c, 1), sink);
  assert(got.size() == 3);
  assert(got[0] == "a");
  assert(got[1].empty());
  assert(got[2] == std::string(300, 'z'));
  assert(!reader.truncated());

  GrpcFrameReader partial;
  partial.feed(wire.substr(0, wire.size() - 1), sink);
  assert(partial.truncated());

  GrpcFrameReader compressed;
  std::string bad = encodeFrame("x");
  bad[0] = 1;
  std::vector<std::string> none;
  compressed.feed(bad, [&](std::string_view f) { none.emplace_back(f); });
  assert(none.empty());
  assert(!compressed.error().empty());

  std::cout << "ok   frame codec: split feeds, empty frame, compressed flag "
               "rejected\n";
}

std::string encodeShots(const std::vector<std::string> &bitstrings,
                        std::size_t elementBytes = 8) {
  std::string out;
  for (const auto &bits : bitstrings)
    for (char c : bits) {
      long long v = c == '1' ? 1 : 0;
      for (std::size_t b = 0; b < elementBytes; ++b)
        out.push_back(static_cast<char>((v >> (8 * b)) & 0xff));
    }
  return out;
}

void checkResultDecoder() {
  assert(bitstringFromMask(0b10, 2) == "01");
  assert(bitstringFromMask(0b01, 2) == "10");
  assert(bitstringFromMask(0b101, 3) == "101");

  // Two iterations of four shots on a 2-bit register, delivered as a single
  // byte run with no boundary marker: the decoder must split them by count.
  const std::vector<std::string> first = {"00", "11", "11", "00"};
  const std::vector<std::string> second = {"10", "10", "10", "01"};
  std::string wire = encodeShots(first) + encodeShots(second);

  QuaResultDecoder decoder("var3", 2, 4);
  for (std::size_t i = 0; i < wire.size(); i += 7)
    decoder.append(std::string_view(wire).substr(i, 7));
  assert(decoder.pendingShots() == 8);
  assert(decoder.pendingBatches() == 2);

  cudaq::CountsDictionary a;
  assert(decoder.takeBatch(a));
  assert(a.size() == 2 && a["00"] == 2 && a["11"] == 2);

  cudaq::CountsDictionary b;
  assert(decoder.takeBatch(b));
  assert(b.size() == 2 && b["10"] == 3 && b["01"] == 1);

  cudaq::CountsDictionary c;
  assert(!decoder.takeBatch(c));
  assert(c.empty());

  // A partial shot is held back rather than decoded short.
  QuaResultDecoder ragged("var2", 3, 2);
  const std::string one = encodeShots({"101"});
  ragged.append(one.substr(0, one.size() - 1));
  assert(ragged.pendingShots() == 0);
  ragged.append(one.substr(one.size() - 1));
  assert(ragged.pendingShots() == 1);

  // Bit i is clbit i, and any nonzero element counts as set.
  QuaResultDecoder wide("var6", 5, 1);
  wide.append(encodeShots({"10010"}));
  auto counts = wide.takeRemaining();
  assert(counts.size() == 1 && counts.count("10010") == 1);

  std::cout << "ok   result decoder: LSB-first bits, count-based iteration "
               "segmentation, partial shots held\n";
}

int checkAgainstMock(int argc, char **argv) {
  const std::string endpoint =
      option(argc, argv, "--endpoint", "127.0.0.1:9515");
  const std::string cluster = option(argc, argv, "--cluster", "");
  const double delay = std::stod(option(argc, argv, "--stream-delay", "0.4"));
  const std::string configPath = option(argc, argv, "--config", "");
  const std::string programPath = option(argc, argv, "--program", "");

  std::cout << GrpcChannel::curlVersionSummary() << "\n";
  if (!GrpcChannel::http2Available()) {
    std::cerr << "FAIL: linked libcurl has no HTTP/2\n";
    return 2;
  }

  GrpcChannel channel(endpoint, cluster);
  channel.setTimeout(30);
  channel.setStreamTimeout(120);
  if (hasFlag(argc, argv, "--verbose"))
    channel.setVerbose(true);

  std::string wire;
  qm::grpc::v2::GetVersionRequest versionReq;
  auto status = channel.invoke("/qm.grpc.v2.QmmService/GetVersion",
                               versionReq.SerializeAsString(), wire);
  if (!status.ok()) {
    std::cerr << "FAIL: GetVersion: " << status.describe() << "\n";
    return 3;
  }
  qm::grpc::v2::GetVersionResponse versionResp;
  assert(versionResp.ParseFromString(wire));
  std::cout << "ok   GetVersion: gateway " << versionResp.success().gateway()
            << "\n";

  std::string machineId = "mock-qm-1";
  if (!configPath.empty()) {
    qm::grpc::v2::OpenQuantumMachineRequest openReq;
    openReq.set_config(readFile(configPath));
    status = channel.invoke("/qm.grpc.v2.QmmService/OpenQuantumMachine",
                            openReq.SerializeAsString(), wire);
    if (!status.ok()) {
      std::cerr << "FAIL: OpenQuantumMachine: " << status.describe() << "\n";
      return 4;
    }
    qm::grpc::v2::OpenQuantumMachineResponse openResp;
    assert(openResp.ParseFromString(wire));
    if (openResp.has_error()) {
      std::cerr << "FAIL: OpenQuantumMachine rejected the config\n";
      return 4;
    }
    machineId = openResp.success().quantum_machine_id();
    std::cout << "ok   OpenQuantumMachine: " << machineId << " ("
              << openReq.config().size() << " byte config)\n";
  }

  std::string programId;
  if (!programPath.empty()) {
    qm::grpc::v2::QmServiceCompileRequest compileReq;
    compileReq.set_quantum_machine_id(machineId);
    compileReq.set_high_level_program(readFile(programPath));
    status = channel.invoke("/qm.grpc.v2.QmService/Compile",
                            compileReq.SerializeAsString(), wire);
    if (!status.ok()) {
      std::cerr << "FAIL: Compile: " << status.describe() << "\n";
      return 5;
    }
    qm::grpc::v2::CompileResponse compileResp;
    assert(compileResp.ParseFromString(wire));
    if (compileResp.has_error()) {
      std::cerr << "FAIL: Compile: " << compileResp.error().details() << "\n";
      return 5;
    }
    programId = compileResp.success().program_id();
    std::cout << "ok   Compile: program_id " << programId << "\n";
  }

  // AddCompiledToQueue and everything downstream plays pulses, so it is only
  // ever sent to the mock.
  std::string jobId = "mock-job";
  if (hasFlag(argc, argv, "--mock")) {
    qm::grpc::v2::QmServiceAddCompiledToQueueRequest queueReq;
    queueReq.set_quantum_machine_id(machineId);
    queueReq.set_program_id(programId);
    status = channel.invoke("/qm.grpc.v2.QmService/AddCompiledToQueue",
                            queueReq.SerializeAsString(), wire);
    if (!status.ok()) {
      std::cerr << "FAIL: AddCompiledToQueue: " << status.describe() << "\n";
      return 6;
    }
    qm::grpc::v2::AddCompiledToQueueResponse queueResp;
    assert(queueResp.ParseFromString(wire));
    jobId = queueResp.success().job_id();
    std::cout << "ok   AddCompiledToQueue: job_id " << jobId << "\n";

    qm::grpc::v2::JobServicePushToInputStreamRequest pushReq;
    pushReq.set_job_id(jobId);
    pushReq.set_stream_name("input_stream_angles");
    pushReq.mutable_fixed_stream_data()->add_data(0.3);
    pushReq.mutable_fixed_stream_data()->add_data(0.7);
    status = channel.invoke("/qm.grpc.v2.JobService/PushToInputStream",
                            pushReq.SerializeAsString(), wire);
    if (!status.ok()) {
      std::cerr << "FAIL: PushToInputStream: " << status.describe() << "\n";
      return 7;
    }
    std::cout << "ok   PushToInputStream: 2 fixed values\n";

    // A method the server does not implement answers HTTP/2 200 with an empty
    // body and grpc-status in the header block instead of the trailers.
    qm::grpc::v2::GetVersionRequest bogus;
    auto missing = channel.invoke("/qm.grpc.v2.QmService/NoSuchMethod",
                                  bogus.SerializeAsString(), wire);
    if (missing.status != 12) {
      std::cerr << "FAIL: expected grpc-status 12 for an unknown method, got "
                << missing.describe() << "\n";
      return 8;
    }
    std::cout << "ok   trailers-only error: HTTP " << missing.httpStatus
              << ", grpc-status 12, empty body\n";

    qm::grpc::v2::GetNamedResultsRequest resultsReq;
    resultsReq.set_job_id(jobId);
    resultsReq.add_outputs()->set_output_name("var3");

    QuaResultDecoder decoder("var3", 2, 4);
    std::vector<double> arrival;
    const auto start = Clock::now();
    int frames = 0;
    int summaryCount = -1;
    status = channel.invokeStreaming(
        "/qm.grpc.v2.JobService/GetNamedResults",
        resultsReq.SerializeAsString(), [&](std::string_view frame) {
          const double t =
              std::chrono::duration<double>(Clock::now() - start).count();
          arrival.push_back(t);
          ++frames;
          qm::grpc::v2::GetNamedResultResponse resp;
          if (!resp.ParseFromArray(frame.data(),
                                   static_cast<int>(frame.size())))
            return;
          if (resp.success().has_data_chunk())
            decoder.append(resp.success().data_chunk().data());
          else if (resp.success().has_data_summary())
            summaryCount = resp.success().data_summary().count();
          std::cout << "     frame " << frames << " at t+" << t << "s, "
                    << frame.size() << " bytes\n";
        });
    if (!status.ok()) {
      std::cerr << "FAIL: GetNamedResults: " << status.describe() << "\n";
      return 9;
    }
    if (frames < 2) {
      std::cerr << "FAIL: expected several frames, got " << frames << "\n";
      return 9;
    }

    const double span = arrival.back() - arrival.front();
    const double expected = delay * (frames - 1);
    if (span < expected * 0.5) {
      std::cerr << "FAIL: all " << frames << " frames arrived within " << span
                << "s but the server spaced them over " << expected
                << "s -- libcurl buffered the response to completion\n";
      return 10;
    }
    std::cout << "ok   incremental delivery: " << frames << " frames spread "
              << "over " << span << "s (server spacing " << expected << "s)\n";

    auto counts = decoder.takeRemaining();
    std::cout << "ok   decoded " << decoder.totalShotsDecoded()
              << " shots, summary count " << summaryCount << ": ";
    for (const auto &[bits, n] : counts)
      std::cout << bits << ":" << n << " ";
    std::cout << "\n";
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  checkFrameCodec();
  checkResultDecoder();
  if (hasFlag(argc, argv, "--offline")) {
    std::cout << "all good (offline checks only)\n";
    return 0;
  }
  const int rc = checkAgainstMock(argc, argv);
  if (rc == 0)
    std::cout << "all good\n";
  return rc;
}
