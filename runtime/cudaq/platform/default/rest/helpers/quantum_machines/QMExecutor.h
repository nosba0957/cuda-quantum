/****************************************************************-*- C++ -*-****
 * Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// Executor for the quantum_machines target: drives an OPX through the QOP gRPC
// API, compiling one QUA program per circuit structure and afterwards putting
// only the variational parameters on the wire. See TODO.md, sections "P3 --
// QMExecutor" and "P5", and GLOSSARY.md.

#pragma once

#include "GrpcCurl.h"
#include "OpnicLink.h"
#include "common/Executor.h"

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cudaq {

/// Everything the executor needs that is not in the kernel, resolved from (in
/// increasing precedence) the target YAML's backend config, the environment,
/// and the running process's own command line.
struct QMSettings {
  std::string endpoint;
  std::string cluster;
  std::string configPath;
  std::string capsPath;
  std::string builderPath;
  std::string python = "python3";
  std::string statePath;
  std::string tempDir;
  std::size_t iterations = 1024;
  int optimizationLevel = 1;
  std::string resetType = "active";
  int resetAttempts = 1;
  /// Feed the variational parameters through a QUA input stream instead of
  /// baking them into the program. The QOP does not currently advance input
  /// streams, so this is off by default.
  bool parametric = false;
  /// "ethernet" or "opnic"; see GLOSSARY.md. OPNIC always compiles once.
  std::string transport = "ethernet";
  std::uint16_t opnicToOpx = 987;
  std::uint16_t opnicFromOpx = 654;
  std::string opnicSerial;
  /// Comma-separated bitstrings: an in-process fake OPX instead of the card.
  std::string opnicFake;
  std::string opnicFakeLog;
  /// Width of one QUA int in a GetNamedResults buffer. Not restated on that
  /// RPC; see QuaResults.h.
  std::size_t resultElementBytes = 8;
  long resultTimeoutSeconds = 120;
};

/// One compiled QUA program plus the queued job that serves it.
struct QMProgram {
  std::string programId;
  std::string jobId;
  std::string inputStream;
  std::size_t inputStreamSize = 1;
  /// Variational parameter indices (the K of pK) that survived transpilation,
  /// one per input-stream slot.
  std::vector<std::size_t> parameterSlots;
  /// Per slot: every coefficient is whole, so the value may be folded by 2*pi.
  std::vector<bool> periodic;
  std::vector<std::pair<std::string, std::size_t>> resultStreams;
  std::size_t shots = 0;
  std::size_t iterations = 0;
  std::size_t pushes = 0;
  std::size_t itemsConsumed = 0;
  /// OPNIC result packets per evaluation; 0 for an Ethernet program.
  std::size_t opnicPackets = 0;
  /// clbits per shot across all result streams, in their order.
  std::size_t resultBits = 0;
};

/// A circuit structure and the values of its variational parameters. The
/// parameter-table pass prints parametric angles as `a*pK+b`, so the QASM text
/// itself is the structure; the values arrive in KernelExecution::user_data.
struct QasmStructure {
  std::string structure;
  std::vector<double> parameters;
};

/// Drops comments and whitespace, so only a real change makes a new structure.
QasmStructure scanQasm(const std::string &qasm,
                       std::vector<double> parameters = {});

/// Folds an angle into [-pi, pi), matching qua_build.py's `_wrap`. QUA `fixed`
/// is 4.28, so a raw optimizer proposal outside that band would overflow once
/// added to the program's own constant term.
double wrapAngle(double radians);

class QMExecutor : public Executor {
public:
  QMExecutor();
  ~QMExecutor() override;

  detail::future execute(std::vector<KernelExecution> &codesToExecute,
                         detail::ExecutionContextType execType =
                             detail::ExecutionContextType::sample,
                         std::vector<char> *rawOutput = nullptr) override;

  /// Opens the quantum machine early instead of on the first cache miss.
  void openQuantumMachine();
  std::string quantumMachineId();

  /// Releases the quantum machine this executor opened. Idempotent, and also
  /// run from the destructor: a machine left open holds this run's
  /// configuration on the controllers and collides with the next user.
  void closeQuantumMachine();

  /// Poll until the QOP reports the job running; input streams reject a push
  /// before that.
  void waitUntilRunning(const std::string &jobId);

  const QMSettings &settings();
  std::size_t cachedPrograms();

private:
  void loadSettings();
  void closeLocked();
  qm::GrpcChannel &openChannel();
  void ensureMachine();
  QMProgram &programFor(const std::string &key, const KernelExecution &code,
                        const QasmStructure &scan);
  QMProgram buildProgram(const KernelExecution &code,
                         const QasmStructure &scan);
  void pushParameters(QMProgram &program, const QasmStructure &scan);
  std::vector<double> streamedValues(const QMProgram &program,
                                     const QasmStructure &scan);
  std::map<std::string, CountsDictionary> receiveOpnic(QMProgram &program);
  void cancelJob(QMProgram &program);
  bool compileOnce() const;
  std::map<std::string, CountsDictionary> fetchCounts(QMProgram &program);
  std::string structuralKey(const QasmStructure &scan);
  std::string quamFingerprint();

  std::mutex sessionMutex;
  QMSettings config;
  bool configLoaded = false;
  std::string fingerprint;
  std::string machineId;
  std::string configBlob;
  std::unique_ptr<qm::GrpcChannel> channel;
  std::map<std::string, QMProgram> cache;
  /// The program whose job was queued last; a job still waiting for
  /// parameters blocks the next one, so it is cancelled first.
  QMProgram *active = nullptr;
  std::unique_ptr<qm::OpnicLink> opnic;
};

} // namespace cudaq
