/****************************************************************************
 * Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.               *
 * All rights reserved.                                                     *
 *                                                                          *
 * This source code and the accompanying materials are made available under *
 * the terms of the Apache License 2.0 which accompanies this distribution.  *
 ****************************************************************************/

// Runnable checks for QMExecutor. The headline one is the P3 gate: two
// cudaq::sample-shaped calls that differ only in rotation angles must produce
// one Compile RPC and two PushToInputStream RPCs, asserted against the mock
// QOP's RPC log rather than against timing.
//
// Build and run via tests/run_p3_checks.sh.

#include "GrpcCurl.h"
#include "QMExecutor.h"
#include "common/KernelExecution.h"
#include "nlohmann/json.hpp"
#include "qm_min.pb.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

using json = nlohmann::json;

namespace {

std::string readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool flag(int argc, char **argv, const std::string &name) {
  for (int i = 1; i < argc; ++i)
    if (name == argv[i])
      return true;
  return false;
}

std::string option(int argc, char **argv, const std::string &flag,
                   const std::string &fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (flag == argv[i])
      return argv[i + 1];
  return fallback;
}

/// Rewrites qaoa_p1's gamma (every rz) and beta (every rx), leaving everything
/// else -- and therefore the structure -- untouched.
std::string respin(const std::string &qasm, double gamma, double beta) {
  auto sub = [](const std::string &text, const std::string &gate, double v) {
    std::ostringstream value;
    value << gate << "(" << std::scientific << v << ")";
    return std::regex_replace(text, std::regex(gate + R"(\([^)]*\))"),
                              value.str());
  };
  return sub(sub(qasm, "rz", gamma), "rx", beta);
}

std::string dropLastRotation(const std::string &qasm) {
  const auto at = qasm.rfind("rx(");
  const auto end = qasm.find('\n', at);
  return qasm.substr(0, at) + qasm.substr(end + 1);
}

cudaq::KernelExecution kernel(const std::string &name,
                              const std::string &code) {
  std::vector<std::size_t> reorder;
  return cudaq::KernelExecution(name, code, std::nullopt, std::nullopt,
                                reorder);
}

struct RpcLog {
  std::vector<json> entries;

  explicit RpcLog(const std::string &path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
      if (!line.empty())
        entries.push_back(json::parse(line));
  }

  std::size_t count(const std::string &method) const {
    std::size_t n = 0;
    for (const auto &e : entries)
      if (e.at("method").get<std::string>().ends_with("/" + method))
        ++n;
    return n;
  }

  std::vector<json> of(const std::string &method) const {
    std::vector<json> out;
    for (const auto &e : entries)
      if (e.at("method").get<std::string>().ends_with("/" + method))
        out.push_back(e);
    return out;
  }
};

void expect(bool condition, const std::string &what) {
  if (!condition)
    throw std::runtime_error(what);
}

void expectEq(std::size_t got, std::size_t want, const std::string &what) {
  if (got != want)
    throw std::runtime_error(what + ": got " + std::to_string(got) + ", want " +
                             std::to_string(want));
}

// -- offline -----------------------------------------------------------------

void checkAngleFolding() {
  expect(std::abs(cudaq::wrapAngle(0.6) - 0.6) < 1e-12, "wrap keeps 0.6");
  expect(std::abs(cudaq::wrapAngle(-0.6) + 0.6) < 1e-12, "wrap keeps -0.6");
  expect(std::abs(cudaq::wrapAngle(7.0) - (7.0 - 2 * M_PI)) < 1e-12,
         "wrap folds 7.0");
  expect(std::abs(cudaq::wrapAngle(-7.0) - (-7.0 + 2 * M_PI)) < 1e-12,
         "wrap folds -7.0");
  for (double x = -40.0; x < 40.0; x += 0.37)
    expect(std::abs(cudaq::wrapAngle(x)) <= M_PI + 1e-12,
           "wrap stays inside the fixed 4.28 range");
  std::cout << "ok   angle folding: every value lands in [-pi, pi)\n";
}

/// The contract between the two halves: theta_k is the k-th rotation angle in
/// QASM instruction order. Checked against the manifests the *real*
/// qua_build.py wrote for the corpus, so a divergence in either scanner shows
/// up here.
void checkAgainstCorpusManifests(const std::string &corpus) {
  const char *names[] = {"bell", "cz_pair", "qaoa_p1", "rz_sweep", "sx_chain"};
  for (const char *name : names) {
    const auto qasm = readFile(corpus + "/" + name + ".qasm");
    const auto manifest =
        json::parse(readFile(corpus + "/" + name + ".manifest.json"));
    const auto scan = cudaq::scanQasm(qasm);

    const auto &angles = manifest.at("angles");
    const auto &values = manifest.at("angle_values");
    expectEq(angles.size(), values.size(),
             std::string(name) + ": manifest angles vs values");
    for (std::size_t i = 0; i < angles.size(); ++i) {
      const std::string theta = angles[i].get<std::string>();
      const std::size_t k = std::stoul(theta.substr(theta.find('_') + 1));
      expect(k < scan.angles.size(),
             std::string(name) + ": " + theta + " is out of range");
      // The QASM carries ~7 significant figures; the manifest carries the
      // parsed double, so they agree only to that.
      expect(std::abs(scan.angles[k] - values[i].get<double>()) < 1e-6,
             std::string(name) + ": " + theta + " is not the " +
                 std::to_string(k) + "-th QASM angle");
    }
    expectEq(scan.angles.size() >= angles.size() ? 1 : 0, 1,
             std::string(name) + ": scanned fewer angles than survived");
    std::cout << "ok   " << name << ": " << scan.angles.size()
              << " rotation angles, " << angles.size()
              << " streamed, ordinals agree with qua_build.py\n";
  }
}

void checkStructuralKey(const std::string &corpus) {
  const auto qaoa = readFile(corpus + "/qaoa_p1.qasm");
  const auto a = cudaq::scanQasm(respin(qaoa, 1.4, 0.6));
  const auto b = cudaq::scanQasm(respin(qaoa, 1.1, 0.9));
  const auto c = cudaq::scanQasm(dropLastRotation(qaoa));

  expect(a.structure == b.structure,
         "circuits differing only in angles share a structure");
  expect(a.angles != b.angles, "the angles themselves still differ");
  expect(a.structure != c.structure,
         "dropping a rotation changes the structure");
  expect(a.structure.find("rz(?)") != std::string::npos,
         "rotation angles are blanked in the structure");
  expect(a.structure.find("1.4") == std::string::npos,
         "no angle literal survives into the structure");
  expect(cudaq::scanQasm(readFile(corpus + "/bell.qasm")).angles.empty(),
         "bell has no rotation angles");

  bool threw = false;
  try {
    cudaq::scanQasm("qreg q[1];\ncreg c[1];\nu3(0.1,0.2,0.3) q[0];\n");
  } catch (const std::exception &) {
    threw = true;
  }
  expect(threw, "a parameterized gate outside qua_build.py's set is rejected");
  std::cout << "ok   structural key: angles blanked, arity kept, unknown "
               "parameterized gates rejected\n";
}

void checkRegistration() {
  expect(cudaq::registry::isRegistered<cudaq::Executor>("quantum_machines"),
         "an Executor is registered under 'quantum_machines'");
  auto executor = cudaq::registry::get<cudaq::Executor>("quantum_machines");
  expect(executor != nullptr, "the registry hands one back");
  expect(dynamic_cast<cudaq::QMExecutor *>(executor.get()) != nullptr,
         "and it is the QMExecutor");
  std::cout << "ok   registry: qpu_utils.cpp will pick QMExecutor up by target "
               "name\n";
}

// -- against the mock --------------------------------------------------------

void checkConnectionReuse(const std::string &endpoint,
                          const std::string &cluster) {
  cudaq::qm::GrpcChannel channel(endpoint, cluster);
  ::qm::grpc::v2::GetVersionRequest request;
  std::string wire;
  const auto first = channel.invoke("/qm.grpc.v2.QmmService/GetVersion",
                                    request.SerializeAsString(), wire);
  expect(first.ok(), "first GetVersion: " + first.describe());
  expectEq(first.newConnections, 1, "the first call opens one connection");
  for (int i = 0; i < 4; ++i) {
    const auto next = channel.invoke("/qm.grpc.v2.QmmService/GetVersion",
                                     request.SerializeAsString(), wire);
    expect(next.ok(), "repeat GetVersion: " + next.describe());
    expectEq(next.newConnections, 0,
             "call " + std::to_string(i + 2) + " reuses the connection");
  }
  std::cout << "ok   channel: one TCP connect for five RPCs (was one connect "
               "per RPC)\n";
}

cudaq::sample_result run(cudaq::QMExecutor &executor,
                         std::vector<cudaq::KernelExecution> codes,
                         cudaq::detail::ExecutionContextType type) {
  return executor.execute(codes, type).get();
}

void checkGate(const std::string &corpus, const std::string &logPath) {
  const auto qaoa = readFile(corpus + "/qaoa_p1.qasm");
  cudaq::QMExecutor executor;
  executor.setShots(4);

  auto first = run(executor, {kernel("qaoa", respin(qaoa, 1.4, 0.6))},
                   cudaq::detail::ExecutionContextType::sample);
  auto second = run(executor, {kernel("qaoa", respin(qaoa, 1.1, 0.9))},
                    cudaq::detail::ExecutionContextType::sample);

  expectEq(first.to_map().size(), 4, "first call returns four distinct shots");
  expectEq(first.to_map("var6").size(), 4, "and names the creg register too");
  expect(first.to_map().count("10101") == 1,
         "clbit-0-first bitstrings survive the round trip");
  expectEq(second.to_map().size(), 4, "second call returns counts as well");

  {
    const RpcLog log(logPath);
    expectEq(log.count("Compile"), 1, "GATE: one Compile for two calls");
    expectEq(log.count("PushToInputStream"), 2,
             "GATE: one PushToInputStream per call");
    expectEq(log.count("OpenQuantumMachine"), 1, "one OpenQuantumMachine");
    expectEq(log.count("AddCompiledToQueue"), 1, "one queued job");
    expectEq(log.count("GetNamedResults"), 2, "one result fetch per call");

    const auto pushes = log.of("PushToInputStream");
    for (const auto &push : pushes) {
      expect(push.at("stream_name") == "input_stream_angles",
             "the mangled input-stream name is used");
      expect(push.at("data_kind") == "fixed_stream_data", "fixed payload");
      expectEq(push.at("values").size(), 9, "nine slots, one per rotation");
    }
    const auto a = pushes[0].at("values").get<std::vector<double>>();
    const auto b = pushes[1].at("values").get<std::vector<double>>();
    for (int i = 0; i < 4; ++i) {
      expect(std::abs(a[i] - 1.4) < 1e-6, "push 1 slot " + std::to_string(i));
      expect(std::abs(b[i] - 1.1) < 1e-6, "push 2 slot " + std::to_string(i));
    }
    for (int i = 4; i < 9; ++i) {
      expect(std::abs(a[i] - 0.6) < 1e-6, "push 1 slot " + std::to_string(i));
      expect(std::abs(b[i] - 0.9) < 1e-6, "push 2 slot " + std::to_string(i));
    }
    const auto fetches = log.of("GetNamedResults");
    expect(!fetches[0].at("ranges")[0].is_null(),
           "GetNamedResults carries a range");
    expectEq(fetches[0].at("ranges")[0][1].get<std::size_t>(), 3,
             "the range covers exactly this batch's shots");
    expectEq(fetches[1].at("ranges")[0][0].get<std::size_t>(), 4,
             "the second batch starts where the first stopped");
    std::cout << "ok   GATE: 1 Compile, 2 PushToInputStream, angles land in "
                 "slot order\n";
  }

  // A structurally different circuit is a cache miss and compiles again.
  run(executor, {kernel("qaoa8", dropLastRotation(respin(qaoa, 1.4, 0.6)))},
      cudaq::detail::ExecutionContextType::sample);
  {
    const RpcLog log(logPath);
    expectEq(log.count("Compile"), 2, "a new structure compiles again");
    expectEq(log.count("PushToInputStream"), 3, "and pushes its own angles");
    expectEq(log.count("OpenQuantumMachine"), 1,
             "but the quantum machine is opened only once");
    expectEq(log.of("PushToInputStream")[2].at("values").size(), 8,
             "the shorter circuit pushes eight slots");
    std::cout << "ok   cache miss: a different structure recompiles, the same "
                 "one never does\n";
  }
  expectEq(executor.cachedPrograms(), 2, "two structures are cached");

  // Every entry of codesToExecute is submitted -- the bug the cloud helper's
  // createJob has, where only circuitCodes[0] is sent.
  auto observed = run(executor,
                      {kernel("term0", respin(qaoa, 0.2, 0.3)),
                       kernel("term1", respin(qaoa, 0.4, 0.5))},
                      cudaq::detail::ExecutionContextType::observe);
  expectEq(observed.register_names().size(), 2, "both terms come back");
  expectEq(observed.to_map("term0").size(), 4, "term0 has counts");
  expectEq(observed.to_map("term1").size(), 4, "term1 has counts");
  {
    const RpcLog log(logPath);
    expectEq(log.count("Compile"), 2, "the shared structure is not recompiled");
    expectEq(log.count("PushToInputStream"), 5, "one push per term");
    const auto pushes = log.of("PushToInputStream");
    expect(std::abs(pushes[3].at("values")[0].get<double>() - 0.2) < 1e-6,
           "term0's angles");
    expect(std::abs(pushes[4].at("values")[0].get<double>() - 0.4) < 1e-6,
           "term1's angles");
    std::cout << "ok   multi-term: all of codesToExecute is submitted\n";
  }

  executor.closeQuantumMachine();
  {
    const RpcLog log(logPath);
    expectEq(log.count("Close"), 1, "the quantum machine is released");
    expect(log.of("Close")[0].at("quantum_machine_id") ==
               log.of("Compile")[0].at("quantum_machine_id"),
           "the machine that was opened is the one that is closed");
    std::cout << "ok   teardown: the quantum machine is closed on the QOP\n";
  }
  executor.closeQuantumMachine();
  expectEq(RpcLog(logPath).count("Close"), 1, "closing twice is idempotent");
}

/// The only thing this sends to a real QOP is what P1 and P2 already sent --
/// OpenQuantumMachine and Compile are non-disruptive -- plus the Close that P2
/// was missing. AddCompiledToQueue plays pulses and is never reached here.
void checkLive(const std::string &corpus, const std::string &endpoint,
               const std::string &cluster) {
  checkConnectionReuse(endpoint, cluster);

  cudaq::QMExecutor executor;
  executor.openQuantumMachine();
  const auto machine = executor.quantumMachineId();
  expect(!machine.empty(), "the QOP opened a quantum machine");
  std::cout << "ok   live OpenQuantumMachine: " << machine << "\n";

  cudaq::qm::GrpcChannel channel(endpoint, cluster);
  ::qm::grpc::v2::QmServiceCompileRequest compile;
  compile.set_quantum_machine_id(machine);
  compile.set_high_level_program(readFile(corpus + "/bell.golden.pb"));
  std::string wire;
  const auto status = channel.invoke("/qm.grpc.v2.QmService/Compile",
                                     compile.SerializeAsString(), wire);
  expect(status.ok(), "live Compile: " + status.describe());
  ::qm::grpc::v2::CompileResponse response;
  expect(response.ParseFromString(wire), "live Compile reply parses");
  expect(!response.has_error(), "live Compile: " + response.error().details());
  std::cout << "ok   live Compile: program_id "
            << response.success().program_id() << "\n";

  executor.closeQuantumMachine();
  expect(executor.quantumMachineId().empty(), "the machine id is released");
  std::cout << "ok   live Close: " << machine << " released\n";
}

} // namespace

int main(int argc, char **argv) {
  const std::string corpus = option(argc, argv, "--corpus", "tests/corpus");
  const std::string endpoint =
      option(argc, argv, "--qm-endpoint", "127.0.0.1:9516");
  const std::string cluster = option(argc, argv, "--qm-cluster", "");
  const std::string logPath = option(argc, argv, "--rpc-log", "");

  try {
    checkAngleFolding();
    checkStructuralKey(corpus);
    checkAgainstCorpusManifests(corpus);
    checkRegistration();
    if (flag(argc, argv, "--live")) {
      checkLive(corpus, endpoint, cluster);
      std::cout << "all good (live sanity pass)\n";
      return 0;
    }
    // Anything past here queues a job. Require the operator to say so.
    if (!flag(argc, argv, "--mock") || logPath.empty()) {
      std::cout << "all good (offline checks only)\n";
      return 0;
    }
    checkConnectionReuse(endpoint, cluster);
    checkGate(corpus, logPath);
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "all good\n";
  return 0;
}
