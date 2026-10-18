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
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>
#include <thread>

namespace pb = ::qm::grpc::v2;

namespace cudaq {

namespace {

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

bool hasFlag(const std::string &flag) {
  for (const auto &arg : processArgs())
    if (arg == flag)
      return true;
  return false;
}

} // namespace

/// Loads the OPNIC plugin that sits next to this library. It is separate so
/// that nothing else needs libopnic at run time.
std::unique_ptr<qm::OpnicLink>
qm::OpnicLink::open(std::uint16_t toOpx, std::uint16_t fromOpx,
                    const std::string &serial, const std::string &fakeResults,
                    const std::string &fakeLog) {
  Dl_info self{};
  dladdr(reinterpret_cast<void *>(&qm::OpnicLink::open), &self);
  const auto path = std::filesystem::path(self.dli_fname ? self.dli_fname : "")
                        .parent_path() /
                    "libcudaq-qm-opnic.so";
  void *plugin = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!plugin)
    throw std::runtime_error(
        "quantum_machines: the OPNIC transport is unavailable: " +
        std::string(dlerror()) +
        ". It needs a build that found QM's opnic SDK, and libopnic at run "
        "time.");
  using Open = qm::OpnicLink *(*)(std::uint16_t, std::uint16_t, const char *,
                                  const char *, const char *);
  auto open = reinterpret_cast<Open>(dlsym(plugin, "cudaq_qm_opnic_open"));
  if (!open)
    throw std::runtime_error("quantum_machines: " + path.string() +
                             " has no cudaq_qm_opnic_open");
  return std::unique_ptr<qm::OpnicLink>(open(
      toOpx, fromOpx, serial.c_str(), fakeResults.c_str(), fakeLog.c_str()));
}

double wrapAngle(double radians) {
  constexpr double twoPi = 2.0 * M_PI;
  return radians - twoPi * std::floor((radians + M_PI) / twoPi);
}

QasmStructure scanQasm(const std::string &qasm,
                       std::vector<double> parameters) {
  QasmStructure out;
  out.parameters = std::move(parameters);
  std::string text;
  for (std::size_t i = 0; i < qasm.size();) {
    if (qasm[i] == '/' && i + 1 < qasm.size() && qasm[i + 1] == '/') {
      while (i < qasm.size() && qasm[i] != '\n')
        ++i;
    } else {
      text.push_back(qasm[i++]);
    }
  }
  out.structure = squeeze(text);
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
  if (auto v = get("parametric"); !v.empty())
    config.parametric = v != "0" && v != "false" && v != "off";
  if (auto v = get("transport"); !v.empty())
    config.transport = v;
  if (hasFlag("--qm-transport-opnic"))
    config.transport = "opnic";
  if (config.transport != "ethernet" && config.transport != "opnic")
    throw std::runtime_error("quantum_machines: transport '" +
                             config.transport +
                             "' is neither 'ethernet' nor 'opnic'");
  if (auto v = get("opnic-in"); !v.empty())
    config.opnicToOpx = std::stoi(v);
  if (auto v = get("opnic-out"); !v.empty())
    config.opnicFromOpx = std::stoi(v);
  config.opnicSerial = get("opnic-serial");
  config.opnicFake = get("opnic-fake");
  config.opnicFakeLog = get("opnic-fake-log");
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
  CUDAQ_INFO("quantum_machines: QOP {} cluster '{}' config {} builder {} "
             "transport {}",
             config.endpoint, config.cluster, config.configPath,
             config.builderPath, config.transport);
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
  // Baked-in parameters are part of the compiled program, so two evaluations
  // differ in program as well as in values unless the values are streamed.
  if (!compileOnce()) {
    ss << std::setprecision(17);
    for (double value : scan.parameters)
      ss << value << ',';
    ss << '|';
  }
  ss << scan.structure << "|transport=" << config.transport
     << "|shots=" << shots << "|iterations=" << config.iterations
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
  if (response.has_error()) {
    std::string detail;
    const auto append = [&detail](const char *kind, const auto &messages) {
      for (const auto &m : messages)
        detail += "\n  " + std::string(kind) + " [" + m.group() + "] " +
                  m.path() + ": " + m.message();
    };
    append("config", response.error().config_validation_errors());
    append("physical", response.error().physical_validation_errors());
    throw std::runtime_error(
        "quantum_machines: the QOP rejected the QuaConfig" + detail);
  }
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
  closeLocked();
}

void QMExecutor::closeLocked() {
  if (machineId.empty() || !channel)
    return;
  if (active && !active->jobId.empty() && active->pushes < active->iterations)
    try {
      cancelJob(*active);
    } catch (const std::exception &e) {
      CUDAQ_WARN("quantum_machines: {}", e.what());
    }
  active = nullptr;
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

void QMExecutor::waitUntilRunning(const std::string &jobId) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (;;) {
    pb::JobServiceGetJobStatusRequest request;
    request.set_job_id(jobId);
    std::string wire;
    auto status = openChannel().invoke("/qm.grpc.v2.JobService/GetJobStatus",
                                       request.SerializeAsString(), wire);
    if (!status.ok())
      throw std::runtime_error("quantum_machines: GetJobStatus failed: " +
                               status.describe());
    pb::JobServiceGetJobStatusResponse response;
    if (!response.ParseFromString(wire))
      throw std::runtime_error(
          "quantum_machines: GetJobStatus returned an unparseable reply");
    if (response.has_error())
      throw std::runtime_error("quantum_machines: GetJobStatus failed: " +
                               response.error().details());

    const auto state = response.success().status();
    if (state == pb::JOB_EXECUTION_STATUS_RUNNING)
      return;
    if (state != pb::JOB_EXECUTION_STATUS_UNSET &&
        state != pb::JOB_EXECUTION_STATUS_UNKNOWN &&
        state != pb::JOB_EXECUTION_STATUS_PENDING &&
        state != pb::JOB_EXECUTION_STATUS_LOADING)
      throw std::runtime_error("quantum_machines: job " + jobId +
                               " reached status " + std::to_string(state) +
                               " without ever running");
    if (std::chrono::steady_clock::now() > deadline)
      throw std::runtime_error("quantum_machines: job " + jobId +
                               " did not start within 60s (status " +
                               std::to_string(state) + ")");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

QMProgram QMExecutor::buildProgram(const KernelExecution &code,
                                   const QasmStructure &scan) {
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
  if (config.transport == "opnic")
    cmd << " --opnic-in " << config.opnicToOpx << " --opnic-out "
        << config.opnicFromOpx;
  else if (!config.parametric && !scan.parameters.empty()) {
    std::ostringstream values;
    values << std::setprecision(17);
    for (std::size_t k = 0; k < scan.parameters.size(); ++k)
      values << (k ? "," : "") << scan.parameters[k];
    cmd << " --bind " << shellQuote(values.str());
  }
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
  for (const auto &name : manifest.at("angles"))
    program.parameterSlots.push_back(
        std::stoul(name.get<std::string>().substr(1)));
  program.periodic = manifest.at("periodic").get<std::vector<bool>>();
  if (manifest.contains("opnic")) {
    const auto &o = manifest.at("opnic");
    if (o.at("max_params") != qm::OpnicMaxParams ||
        o.at("shots_per_packet") != qm::OpnicShotsPerPacket ||
        o.at("max_bits") != qm::OpnicMaxBits)
      throw std::runtime_error("quantum_machines: qua_build.py's OPNIC packet "
                               "shapes differ from this build's: " +
                               o.dump());
    program.opnicPackets = o.at("packets").get<std::size_t>();
  }
  for (const auto &stream : manifest.at("result_streams")) {
    program.resultStreams.emplace_back(stream.at("name").get<std::string>(),
                                       stream.at("size").get<std::size_t>());
    program.resultBits += program.resultStreams.back().second;
  }
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
    it = cache.emplace(key, buildProgram(code, scan)).first;

  QMProgram &program = it->second;
  for (std::size_t slot : program.parameterSlots)
    if (slot >= scan.parameters.size())
      throw std::runtime_error(
          "quantum_machines: the program reads parameter p" +
          std::to_string(slot) + " but this evaluation has only " +
          std::to_string(scan.parameters.size()));

  // A job serves `iterations` pushes and then ends. Requeueing the already
  // compiled program costs one RPC and keeps Compile off the hot path.
  if (program.jobId.empty() || program.pushes >= program.iterations) {
    if (active && active != &program && !active->jobId.empty() &&
        active->pushes < active->iterations)
      cancelJob(*active);
    // OPNIC streams must exist before the job that uses them starts.
    if (program.opnicPackets && !opnic)
      opnic = qm::OpnicLink::open(config.opnicToOpx, config.opnicFromOpx,
                                  config.opnicSerial, config.opnicFake,
                                  config.opnicFakeLog);
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
    // Only a push needs the job running. A stream-free job may already be
    // PROCESSING by the time we could ask.
    if (!program.inputStream.empty() || program.opnicPackets)
      waitUntilRunning(program.jobId);
    active = &program;
    program.pushes = 0;
    program.itemsConsumed = 0;
    CUDAQ_INFO("quantum_machines: queued program {} as job {}",
               program.programId, program.jobId);
  }
  return program;
}

void QMExecutor::pushParameters(QMProgram &program, const QasmStructure &scan) {
  if (program.inputStream.empty() && !program.opnicPackets) {
    // Nothing streamed, so no stream gates the job: it runs its shots on start.
    ++program.pushes;
    return;
  }
  const auto values = streamedValues(program, scan);
  if (program.opnicPackets) {
    opnic->expect(program.shots, program.opnicPackets);
    opnic->send(values);
    ++program.pushes;
    return;
  }
  pb::JobServicePushToInputStreamRequest request;
  request.set_job_id(program.jobId);
  request.set_stream_name(program.inputStream);
  auto *data = request.mutable_fixed_stream_data();
  for (double value : values)
    data->add_data(value);
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

bool QMExecutor::compileOnce() const {
  return config.parametric || config.transport == "opnic";
}

std::vector<double> QMExecutor::streamedValues(const QMProgram &program,
                                               const QasmStructure &scan) {
  std::vector<double> values;
  for (std::size_t i = 0; i < program.parameterSlots.size(); ++i) {
    double value = scan.parameters[program.parameterSlots[i]];
    // Whole coefficients make a 2*pi shift invisible to every angle.
    if (i < program.periodic.size() && program.periodic[i])
      value = wrapAngle(value);
    if (std::abs(value) >= 8.0)
      throw std::runtime_error(
          "quantum_machines: parameter p" +
          std::to_string(program.parameterSlots[i]) + " = " +
          std::to_string(value) +
          " does not fit QUA fixed (|x| < 8) and cannot be folded, because "
          "an angle uses it with a fractional coefficient");
    values.push_back(value);
  }
  return values;
}

std::map<std::string, CountsDictionary>
QMExecutor::receiveOpnic(QMProgram &program) {
  const auto bits = opnic->receive(program.opnicPackets);
  std::map<std::string, CountsDictionary> counts;
  for (std::size_t shot = 0; shot < program.shots; ++shot) {
    const auto *row = bits.data() + shot * qm::OpnicMaxBits;
    std::size_t offset = 0;
    for (const auto &[name, size] : program.resultStreams) {
      std::string outcome(size, '0');
      for (std::size_t i = 0; i < size; ++i)
        outcome[i] = row[offset + i] ? '1' : '0';
      ++counts[name][outcome];
      offset += size;
    }
  }
  return counts;
}

void QMExecutor::cancelJob(QMProgram &program) {
  pb::CancelRequest request;
  request.set_job_id(program.jobId);
  std::string wire;
  auto status = openChannel().invoke("/qm.grpc.v2.JobService/Cancel",
                                     request.SerializeAsString(), wire);
  pb::CancelResponse response;
  if (!status.ok() || !response.ParseFromString(wire) || response.has_error())
    throw std::runtime_error(
        "quantum_machines: could not cancel job " + program.jobId + ": " +
        (status.ok() ? response.error().details() : status.describe()));
  CUDAQ_INFO("quantum_machines: cancelled job {}", program.jobId);
  program.jobId.clear();
  if (active == &program)
    active = nullptr;
}

std::map<std::string, CountsDictionary>
QMExecutor::fetchCounts(QMProgram &program) {
  if (program.opnicPackets)
    return receiveOpnic(program);
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
    if (!status.ok()) {
      // The QOP registers a job's result streams a moment after the job
      // starts, and reports the name as missing until it has.
      const std::string described = status.describe();
      if (described.find("cannot find result") != std::string::npos &&
          std::chrono::steady_clock::now() <= deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      throw std::runtime_error("quantum_machines: GetNamedResults failed: " +
                               described);
    }

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

  return std::async(std::launch::async, [this, codes, isObserve,
                                         single]() mutable {
    std::lock_guard<std::mutex> guard(sessionMutex);
    try {
      loadSettings();

      std::vector<ExecutionResult> results;
      for (auto &code : codes) {
        if (!code.mapping_reorder_idx.empty())
          CUDAQ_WARN("quantum_machines: kernel {} carries a mapping reorder "
                     "index, which this target does not apply",
                     code.name);
        std::vector<double> parameters;
        if (const auto &ud = code.user_data.get(); ud.contains("parameters"))
          parameters = ud.at("parameters").get<std::vector<double>>();
        const auto scan = scanQasm(code.code, std::move(parameters));
        auto &program = programFor(structuralKey(scan), code, scan);
        pushParameters(program, scan);
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
    } catch (...) {
      try {
        closeLocked();
      } catch (...) {
      }
      throw;
    }
  });
}

} // namespace cudaq

CUDAQ_REGISTER_TYPE(cudaq::Executor, cudaq::QMExecutor, quantum_machines)
