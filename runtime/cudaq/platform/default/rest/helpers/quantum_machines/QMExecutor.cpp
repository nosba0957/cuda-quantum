/*******************************************************************************
 * Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// Implementation of the quantum_machines executor. See QMExecutor.h.

#include "QMExecutor.h"
#include "QuaResults.h"
#include "common/KernelExecution.h"
#include "common/ServerHelper.h"
#include "nlohmann/json.hpp"
#include "qm_min.pb.h"
#include "cudaq/runtime/logger/logger.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <sstream>
#include <thread>

namespace pb = ::qm::grpc::v2;

namespace cudaq {

namespace {

/// The rotation op names qua_build.py's `parameterize` assigns theta slots to.
/// Kept identical to its `_ROT`: the k-th angle here must be the k-th angle
/// there, or streamed values land on the wrong gates.
const std::set<std::string> &rotations() {
  static const std::set<std::string> names = {
      "rz", "rx", "ry", "p", "u1", "rzz", "crz", "crx", "cry", "cu1"};
  return names;
}

std::string readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("quantum_machines: cannot read " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string squeeze(const std::string &s) {
  std::string out;
  for (char c : s)
    if (!std::isspace(static_cast<unsigned char>(c)))
      out.push_back(c);
  return out;
}

std::string trim(const std::string &s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

double parseAngle(const std::string &token, const std::string &gate) {
  const std::string t = trim(token);
  const char *begin = t.c_str();
  char *end = nullptr;
  const double v = std::strtod(begin, &end);
  if (end == begin || *end != '\0')
    throw std::runtime_error(
        "quantum_machines: gate '" + gate + "' has a non-numeric angle '" + t +
        "'; angles must be plain numbers so they can be streamed");
  return v;
}

std::vector<std::string> splitTopLevel(const std::string &s, char sep) {
  std::vector<std::string> out;
  int depth = 0;
  std::string current;
  for (char c : s) {
    if (c == '(' || c == '[')
      ++depth;
    else if (c == ')' || c == ']')
      --depth;
    if (c == sep && depth == 0) {
      out.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  out.push_back(current);
  return out;
}

std::string fnv1a(const std::string &s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  std::ostringstream ss;
  ss << std::hex << h;
  return ss.str();
}

std::string shellQuote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out.push_back(c);
  }
  return out + "'";
}

/// The produced binary has no argument parser of its own, so a runtime flag
/// such as `--qm-config` has to be read back off the process itself.
const std::vector<std::string> &processArgs() {
  static const std::vector<std::string> args = [] {
    std::vector<std::string> v;
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    if (!in)
      return v;
    const std::string all((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    std::size_t start = 0;
    for (std::size_t i = 0; i < all.size(); ++i)
      if (all[i] == '\0') {
        v.emplace_back(all, start, i - start);
        start = i + 1;
      }
    return v;
  }();
  return args;
}

std::string upperUnderscore(const std::string &key) {
  std::string out;
  for (char c : key)
    out.push_back(c == '-' ? '_'
                           : static_cast<char>(
                                 std::toupper(static_cast<unsigned char>(c))));
  return out;
}

std::string lowerUnderscore(const std::string &key) {
  std::string out;
  for (char c : key)
    out.push_back(c == '-' ? '_' : c);
  return out;
}

std::string lookupSetting(const std::string &key,
                          const BackendConfig &backend) {
  const std::string flag = "--qm-" + key;
  const auto &args = processArgs();
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == flag && i + 1 < args.size())
      return args[i + 1];
    if (args[i].rfind(flag + "=", 0) == 0)
      return args[i].substr(flag.size() + 1);
  }
  if (const char *env =
          std::getenv(("CUDAQ_QM_" + upperUnderscore(key)).c_str()))
    return env;
  const auto it = backend.find("qm_" + lowerUnderscore(key));
  return it == backend.end() ? std::string() : it->second;
}

} // namespace

double wrapAngle(double radians) {
  constexpr double twoPi = 2.0 * M_PI;
  return radians - twoPi * std::floor((radians + M_PI) / twoPi);
}

QasmStructure scanQasm(const std::string &qasm) {
  QasmStructure out;
  std::string text;
  for (std::size_t i = 0; i < qasm.size();) {
    if (qasm[i] == '/' && i + 1 < qasm.size() && qasm[i + 1] == '/') {
      while (i < qasm.size() && qasm[i] != '\n')
        ++i;
    } else {
      text.push_back(qasm[i++]);
    }
  }

  std::size_t pos = 0;
  while (pos < text.size()) {
    const auto end = text.find(';', pos);
    if (end == std::string::npos)
      break;
    const std::string stmt = trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (stmt.empty())
      continue;

    std::size_t n = 0;
    while (
        n < stmt.size() &&
        (std::isalnum(static_cast<unsigned char>(stmt[n])) || stmt[n] == '_'))
      ++n;
    const std::string name = stmt.substr(0, n);

    if (name == "OPENQASM" || name == "include" || name == "qreg" ||
        name == "creg" || name == "barrier" || name == "measure" ||
        name == "reset") {
      out.structure += squeeze(stmt) + ";";
      continue;
    }
    if (name.empty() || name == "gate" || name == "opaque" || name == "if")
      throw std::runtime_error(
          "quantum_machines: unsupported OpenQASM statement '" + stmt + "'");

    std::size_t p = n;
    while (p < stmt.size() && std::isspace(static_cast<unsigned char>(stmt[p])))
      ++p;
    if (p >= stmt.size() || stmt[p] != '(') {
      out.structure += squeeze(stmt) + ";";
      continue;
    }

    const auto close = stmt.rfind(')');
    if (close == std::string::npos || close < p)
      throw std::runtime_error("quantum_machines: unbalanced parentheses in '" +
                               stmt + "'");
    if (!rotations().count(name))
      throw std::runtime_error(
          "quantum_machines: parameterized gate '" + name +
          "' is outside qua_build.py's rotation set, so streamed angles would "
          "be misaligned with the compiled program");

    const auto params = splitTopLevel(stmt.substr(p + 1, close - p - 1), ',');
    out.structure += name + "(";
    for (std::size_t k = 0; k < params.size(); ++k) {
      out.angles.push_back(parseAngle(params[k], name));
      out.structure += k ? ",?" : "?";
    }
    out.structure += ")" + squeeze(stmt.substr(close + 1)) + ";";
  }
  return out;
}

QMExecutor::QMExecutor() {
  // Executor leaves serverHelper uninitialized and relies on every caller
  // running setServerHelper first; loadSettings tolerates its absence, so make
  // the absence readable.
  serverHelper = nullptr;
}

QMExecutor::~QMExecutor() {
  try {
    closeQuantumMachine();
  } catch (const std::exception &e) {
    CUDAQ_WARN("quantum_machines: closing the quantum machine failed: {}",
               e.what());
  } catch (...) {
  }
}

void QMExecutor::loadSettings() {
  if (configLoaded)
    return;
  const BackendConfig backend =
      serverHelper ? serverHelper->getConfig() : BackendConfig{};

  auto get = [&](const char *key) { return lookupSetting(key, backend); };

  config.endpoint = get("endpoint");
  if (config.endpoint.empty()) {
    // The target's `url` argument predates this executor and may carry a
    // scheme; the gRPC path wants a bare host:port.
    auto it = backend.find("url");
    if (it != backend.end()) {
      config.endpoint = it->second;
      const auto scheme = config.endpoint.find("://");
      if (scheme != std::string::npos)
        config.endpoint = config.endpoint.substr(scheme + 3);
      while (!config.endpoint.empty() && config.endpoint.back() == '/')
        config.endpoint.pop_back();
    }
  }
  config.cluster = get("cluster");
  config.configPath = get("config");
  config.capsPath = get("caps");
  config.builderPath = get("builder");
  config.statePath = get("state");
  config.tempDir = get("tmpdir");

  if (auto v = get("python"); !v.empty())
    config.python = v;
  if (auto v = get("iterations"); !v.empty())
    config.iterations = std::stoul(v);
  if (auto v = get("opt-level"); !v.empty())
    config.optimizationLevel = std::stoi(v);
  if (auto v = get("reset"); !v.empty())
    config.resetType = v;
  if (auto v = get("reset-attempts"); !v.empty())
    config.resetAttempts = std::stoi(v);
  if (auto v = get("element-bytes"); !v.empty())
    config.resultElementBytes = std::stoul(v);
  if (auto v = get("result-timeout"); !v.empty())
    config.resultTimeoutSeconds = std::stol(v);

  if (config.statePath.empty())
    if (const char *env = std::getenv("QUAM_STATE_PATH"))
      config.statePath = env;
  if (config.builderPath.empty())
    config.builderPath = "qua_build.py";
  if (config.capsPath.empty() && !config.configPath.empty() &&
      std::filesystem::exists(config.configPath + ".caps.json"))
    config.capsPath = config.configPath + ".caps.json";
  if (config.tempDir.empty())
    config.tempDir = std::filesystem::temp_directory_path().string();

  if (config.endpoint.empty())
    throw std::runtime_error(
        "quantum_machines: no QOP endpoint; pass --qm-endpoint host:port");
  if (config.configPath.empty())
    throw std::runtime_error(
        "quantum_machines: no QuaConfig; pass --qm-config <qua_config.pb>");

  configLoaded = true;
  CUDAQ_INFO("quantum_machines: QOP {} cluster '{}' config {} builder {}",
             config.endpoint, config.cluster, config.configPath,
             config.builderPath);
}

const QMSettings &QMExecutor::settings() {
  std::lock_guard<std::mutex> guard(sessionMutex);
  loadSettings();
  return config;
}

std::size_t QMExecutor::cachedPrograms() {
  std::lock_guard<std::mutex> guard(sessionMutex);
  return cache.size();
}

qm::GrpcChannel &QMExecutor::openChannel() {
  if (!channel) {
    if (!qm::GrpcChannel::http2Available())
      throw std::runtime_error(
          "quantum_machines: the linked libcurl has no HTTP/2 (" +
          qm::GrpcChannel::curlVersionSummary() + ")");
    channel =
        std::make_unique<qm::GrpcChannel>(config.endpoint, config.cluster);
    channel->setStreamTimeout(config.resultTimeoutSeconds);
  }
  return *channel;
}

std::string QMExecutor::quamFingerprint() {
  if (!fingerprint.empty())
    return fingerprint;
  std::string material = config.statePath;
  for (const char *name : {"state.json", "wiring.json"}) {
    const auto path = std::filesystem::path(config.statePath) / name;
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
      material += readFile(path.string());
  }
  // Length, never bytes: QuaConfig serialization is not deterministic across
  // runs (protobuf map ordering), but its size is.
  std::error_code ec;
  material += "|" + config.configPath + "|" +
              std::to_string(std::filesystem::file_size(config.configPath, ec));
  fingerprint = fnv1a(material);
  return fingerprint;
}

std::string QMExecutor::structuralKey(const QasmStructure &scan) {
  std::ostringstream ss;
  ss << scan.structure << "|shots=" << shots
     << "|iterations=" << config.iterations
     << "|opt=" << config.optimizationLevel << "|reset=" << config.resetType
     << ':' << config.resetAttempts << "|quam=" << quamFingerprint();
  return ss.str();
}

void QMExecutor::ensureMachine() {
  if (!machineId.empty())
    return;
  if (configBlob.empty())
    configBlob = readFile(config.configPath);

  pb::OpenQuantumMachineRequest request;
  request.set_config(configBlob);
  std::string wire;
  auto status =
      openChannel().invoke("/qm.grpc.v2.QmmService/OpenQuantumMachine",
                           request.SerializeAsString(), wire);
  if (!status.ok())
    throw std::runtime_error("quantum_machines: OpenQuantumMachine failed: " +
                             status.describe());
  pb::OpenQuantumMachineResponse response;
  if (!response.ParseFromString(wire))
    throw std::runtime_error(
        "quantum_machines: OpenQuantumMachine returned an unparseable reply");
  if (response.has_error())
    throw std::runtime_error(
        "quantum_machines: the QOP rejected the QuaConfig (" +
        std::to_string(response.error().config_validation_errors_size()) +
        " config and " +
        std::to_string(response.error().physical_validation_errors_size()) +
        " physical validation errors)");
  machineId = response.success().quantum_machine_id();
  CUDAQ_INFO("quantum_machines: opened quantum machine {} ({} byte config)",
             machineId, configBlob.size());
}

void QMExecutor::openQuantumMachine() {
  std::lock_guard<std::mutex> guard(sessionMutex);
  loadSettings();
  ensureMachine();
}

std::string QMExecutor::quantumMachineId() {
  std::lock_guard<std::mutex> guard(sessionMutex);
  return machineId;
}

void QMExecutor::closeQuantumMachine() {
  std::lock_guard<std::mutex> guard(sessionMutex);
  if (machineId.empty() || !channel)
    return;
  pb::QmServiceCloseRequest request;
  request.set_quantum_machine_id(machineId);
  std::string wire;
  auto status = channel->invoke("/qm.grpc.v2.QmService/Close",
                                request.SerializeAsString(), wire);
  const std::string closed = machineId;
  machineId.clear();
  cache.clear();
  if (!status.ok())
    throw std::runtime_error("quantum_machines: Close failed for " + closed +
                             ": " + status.describe());
  CUDAQ_INFO("quantum_machines: closed quantum machine {}", closed);
}

QMProgram QMExecutor::buildProgram(const KernelExecution &code) {
  const auto dir = std::filesystem::path(config.tempDir) /
                   ("cudaq-qm-" + fnv1a(code.name + code.code));
  std::filesystem::create_directories(dir);
  const auto qasmPath = (dir / "circuit.qasm").string();
  const auto programPath = (dir / "program.pb").string();
  const auto manifestPath = (dir / "manifest.json").string();
  const auto logPath = (dir / "qua_build.log").string();
  {
    std::ofstream out(qasmPath, std::ios::binary);
    out << code.code;
  }

  std::ostringstream cmd;
  cmd << shellQuote(config.python) << ' ' << shellQuote(config.builderPath)
      << " --qasm " << shellQuote(qasmPath) << " --output "
      << shellQuote(programPath) << " --manifest " << shellQuote(manifestPath)
      << " --shots " << shots << " --iterations " << config.iterations
      << " --optimization-level " << config.optimizationLevel
      << " --reset-type " << shellQuote(config.resetType)
      << " --reset-max-attempts " << config.resetAttempts;
  if (!config.capsPath.empty())
    cmd << " --caps-file " << shellQuote(config.capsPath);
  if (!config.statePath.empty())
    cmd << " --state-path " << shellQuote(config.statePath);
  cmd << " > " << shellQuote(logPath) << " 2>&1";

  CUDAQ_INFO("quantum_machines: building a QUA program for kernel {}",
             code.name);
  const int rc = std::system(cmd.str().c_str());
  if (rc != 0) {
    std::string log;
    try {
      log = readFile(logPath);
    } catch (...) {
    }
    throw std::runtime_error("quantum_machines: qua_build.py failed (exit " +
                             std::to_string(rc) + "):\n" + log);
  }

  const auto manifest = nlohmann::json::parse(readFile(manifestPath));
  const auto programBytes = readFile(programPath);

  QMProgram program;
  program.inputStream = manifest.at("input_stream").get<std::string>();
  program.inputStreamSize = manifest.at("input_stream_size").get<std::size_t>();
  program.shots = manifest.at("shots").get<std::size_t>();
  program.iterations = manifest.at("iterations").get<std::size_t>();
  for (const auto &name : manifest.at("angles")) {
    const std::string theta = name.get<std::string>();
    program.angleSlots.push_back(std::stoul(theta.substr(theta.find('_') + 1)));
  }
  for (const auto &stream : manifest.at("result_streams"))
    program.resultStreams.emplace_back(stream.at("name").get<std::string>(),
                                       stream.at("size").get<std::size_t>());
  if (program.resultStreams.empty())
    throw std::runtime_error(
        "quantum_machines: the built program has no result stream");

  ensureMachine();

  pb::QmServiceCompileRequest compileReq;
  compileReq.set_quantum_machine_id(machineId);
  compileReq.set_high_level_program(programBytes);
  std::string wire;
  auto status = openChannel().invoke("/qm.grpc.v2.QmService/Compile",
                                     compileReq.SerializeAsString(), wire);
  if (!status.ok())
    throw std::runtime_error("quantum_machines: Compile failed: " +
                             status.describe());
  pb::CompileResponse compileResp;
  if (!compileResp.ParseFromString(wire))
    throw std::runtime_error(
        "quantum_machines: Compile returned an unparseable reply");
  if (compileResp.has_error())
    throw std::runtime_error(
        "quantum_machines: Compile rejected the program: " +
        compileResp.error().details());
  program.programId = compileResp.success().program_id();
  CUDAQ_INFO("quantum_machines: compiled {} bytes as program {}",
             programBytes.size(), program.programId);
  return program;
}

QMProgram &QMExecutor::programFor(const std::string &key,
                                  const KernelExecution &code,
                                  const QasmStructure &scan) {
  auto it = cache.find(key);
  if (it == cache.end())
    it = cache.emplace(key, buildProgram(code)).first;

  QMProgram &program = it->second;
  for (std::size_t slot : program.angleSlots)
    if (slot >= scan.angles.size())
      throw std::runtime_error(
          "quantum_machines: the cached program wants angle theta_" +
          std::to_string(slot) + " but this circuit only has " +
          std::to_string(scan.angles.size()));

  // A job serves `iterations` pushes and then ends. Requeueing the already
  // compiled program costs one RPC and keeps Compile off the hot path.
  if (program.jobId.empty() || program.pushes >= program.iterations) {
    pb::QmServiceAddCompiledToQueueRequest request;
    request.set_quantum_machine_id(machineId);
    request.set_program_id(program.programId);
    std::string wire;
    auto status =
        openChannel().invoke("/qm.grpc.v2.QmService/AddCompiledToQueue",
                             request.SerializeAsString(), wire);
    if (!status.ok())
      throw std::runtime_error("quantum_machines: AddCompiledToQueue failed: " +
                               status.describe());
    pb::AddCompiledToQueueResponse response;
    if (!response.ParseFromString(wire))
      throw std::runtime_error(
          "quantum_machines: AddCompiledToQueue returned an unparseable reply");
    if (response.has_error())
      throw std::runtime_error("quantum_machines: AddCompiledToQueue failed: " +
                               response.error().details());
    program.jobId = response.success().job_id();
    program.pushes = 0;
    program.itemsConsumed = 0;
    CUDAQ_INFO("quantum_machines: queued program {} as job {}",
               program.programId, program.jobId);
  }
  return program;
}

void QMExecutor::pushAngles(QMProgram &program, const QasmStructure &scan) {
  pb::JobServicePushToInputStreamRequest request;
  request.set_job_id(program.jobId);
  request.set_stream_name(program.inputStream);
  auto *data = request.mutable_fixed_stream_data();
  for (std::size_t slot : program.angleSlots)
    data->add_data(wrapAngle(scan.angles[slot]));
  // qua_build.py declares a one-element stream even when nothing survived, so
  // every batch is gated by exactly one push.
  if (program.angleSlots.empty())
    data->add_data(0.0);
  if (static_cast<std::size_t>(data->data_size()) != program.inputStreamSize)
    throw std::runtime_error("quantum_machines: pushing " +
                             std::to_string(data->data_size()) +
                             " values into a stream declared with " +
                             std::to_string(program.inputStreamSize));

  std::string wire;
  auto status = openChannel().invoke("/qm.grpc.v2.JobService/PushToInputStream",
                                     request.SerializeAsString(), wire);
  if (!status.ok())
    throw std::runtime_error("quantum_machines: PushToInputStream failed: " +
                             status.describe());
  pb::PushToInputStreamResponse response;
  if (!response.ParseFromString(wire))
    throw std::runtime_error(
        "quantum_machines: PushToInputStream returned an unparseable reply");
  if (response.has_error())
    throw std::runtime_error("quantum_machines: PushToInputStream failed: " +
                             response.error().details());
  ++program.pushes;
}

std::map<std::string, CountsDictionary>
QMExecutor::fetchCounts(QMProgram &program) {
  std::map<std::string, std::unique_ptr<qm::QuaResultDecoder>> decoders;
  std::map<std::string, int> summaries;
  for (const auto &[name, size] : program.resultStreams) {
    decoders[name] = std::make_unique<qm::QuaResultDecoder>(
        name, size, program.shots, config.resultElementBytes);
    summaries[name] = -1;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(config.resultTimeoutSeconds);
  auto complete = [&] {
    for (const auto &entry : decoders)
      if (entry.second->pendingShots() < program.shots)
        return false;
    return true;
  };

  while (!complete()) {
    pb::GetNamedResultsRequest request;
    request.set_job_id(program.jobId);
    for (const auto &entry : program.resultStreams) {
      const std::string &name = entry.first;
      // The SDK always sends a range; an unbounded request against a live job
      // has no reason to terminate. Bounds are inclusive item indices.
      const std::size_t from =
          program.itemsConsumed + decoders[name]->pendingShots();
      auto *output = request.add_outputs();
      output->set_output_name(name);
      output->mutable_range()->mutable_from()->set_value(
          static_cast<std::int64_t>(from));
      output->mutable_range()->mutable_to()->set_value(
          static_cast<std::int64_t>(program.itemsConsumed + program.shots - 1));
    }

    std::string failure;
    auto status = openChannel().invokeStreaming(
        "/qm.grpc.v2.JobService/GetNamedResults", request.SerializeAsString(),
        [&](std::string_view frame) {
          pb::GetNamedResultResponse response;
          if (!response.ParseFromArray(frame.data(),
                                       static_cast<int>(frame.size()))) {
            failure = "unparseable GetNamedResults frame";
            return;
          }
          if (response.has_error()) {
            failure = response.error().details();
            return;
          }
          const auto &success = response.success();
          // output_name is empty on servers predating per-name chunking; with a
          // single stream requested there is nothing to demultiplex.
          std::string name = success.output_name();
          if (name.empty() && decoders.size() == 1)
            name = decoders.begin()->first;
          const auto it = decoders.find(name);
          if (it == decoders.end()) {
            failure =
                "GetNamedResults returned unrequested output '" + name + "'";
            return;
          }
          if (success.has_data_chunk())
            it->second->append(success.data_chunk().data());
          else if (success.has_data_summary())
            summaries[name] = success.data_summary().count();
        });
    if (!failure.empty())
      throw std::runtime_error("quantum_machines: GetNamedResults: " + failure);
    if (!status.ok())
      throw std::runtime_error("quantum_machines: GetNamedResults failed: " +
                               status.describe());

    if (complete())
      break;
    if (std::chrono::steady_clock::now() > deadline)
      throw std::runtime_error(
          "quantum_machines: job " + program.jobId + " produced only " +
          std::to_string(decoders.begin()->second->pendingShots()) + " of " +
          std::to_string(program.shots) + " shots within " +
          std::to_string(config.resultTimeoutSeconds) + "s");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::map<std::string, CountsDictionary> counts;
  for (auto &[name, decoder] : decoders) {
    // DataSummary.count is the QOP's own item count, and the only free check
    // that resultElementBytes matches the stream's real element width.
    if (summaries[name] >= 0 &&
        static_cast<std::size_t>(summaries[name]) != decoder->pendingShots())
      throw std::runtime_error(
          "quantum_machines: output '" + name + "' decoded " +
          std::to_string(decoder->pendingShots()) + " items but the QOP said " +
          std::to_string(summaries[name]) +
          "; the result element width is wrong");
    if (!decoder->takeBatch(counts[name]))
      throw std::runtime_error("quantum_machines: output '" + name +
                               "' delivered a short batch");
  }
  program.itemsConsumed += program.shots;
  return counts;
}

detail::future QMExecutor::execute(std::vector<KernelExecution> &codesToExecute,
                                   detail::ExecutionContextType execType,
                                   std::vector<char> *) {
  // rawOutput is ignored, not rejected: the sample path always passes it.
  if (execType == detail::ExecutionContextType::run)
    throw std::runtime_error(
        "quantum_machines: cudaq::run is not supported by this target");
  if (serverHelper)
    serverHelper->setShots(shots);

  const bool isObserve = execType == detail::ExecutionContextType::observe;
  const bool single = codesToExecute.size() == 1;
  std::vector<KernelExecution> codes = codesToExecute;

  return std::async(
      std::launch::async, [this, codes, isObserve, single]() mutable {
        std::lock_guard<std::mutex> guard(sessionMutex);
        loadSettings();

        std::vector<ExecutionResult> results;
        for (auto &code : codes) {
          if (!code.mapping_reorder_idx.empty())
            CUDAQ_WARN("quantum_machines: kernel {} carries a mapping reorder "
                       "index, which this target does not apply",
                       code.name);
          const auto scan = scanQasm(code.code);
          auto &program = programFor(structuralKey(scan), code, scan);
          pushAngles(program, scan);
          auto counts = fetchCounts(program);

          if (single && !isObserve) {
            for (auto &[name, dictionary] : counts)
              results.emplace_back(dictionary, name);
            if (counts.size() == 1)
              results.emplace_back(counts.begin()->second, GlobalRegisterName);
          } else {
            // One register per kernel, because several kernels share one creg
            // name and a per-creg register would have them overwrite each
            // other.
            if (counts.size() != 1)
              throw std::runtime_error("quantum_machines: kernel " + code.name +
                                       " has " + std::to_string(counts.size()) +
                                       " classical registers; multi-kernel "
                                       "submission needs exactly one");
            results.emplace_back(counts.begin()->second, code.name);
          }
        }
        return sample_result(results);
      });
}

} // namespace cudaq

CUDAQ_REGISTER_TYPE(cudaq::Executor, cudaq::QMExecutor, quantum_machines)
