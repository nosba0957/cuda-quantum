/****************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.               *
 * All rights reserved.                                                     *
 *                                                                          *
 * This source code and the accompanying materials are made available under *
 * the terms of the Apache License 2.0 which accompanies this distribution.  *
 ****************************************************************************/

// A minimal gRPC client over libcurl HTTP/2 cleartext, enough for the six QOP
// RPCs the quantum_machines executor calls: unary requests and server-streamed
// responses, with no grpc++ or protobuf runtime involvement.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace cudaq::qm {

/// Outcome of one gRPC call. A call can fail at three levels: libcurl, HTTP, or
/// gRPC, and each is reported separately because they mean different things.
struct GrpcResult {
  long httpStatus = 0;
  /// grpc-status from the header or trailer block; -1 if the server never sent
  /// one, which is itself a protocol violation.
  int status = -1;
  std::string message;
  std::string transportError;

  bool ok() const { return transportError.empty() && status == 0; }
  std::string describe() const;
};

using GrpcFrameHandler = std::function<void(std::string_view)>;

/// Splits a byte stream into gRPC length-prefixed messages, invoking the
/// handler as soon as each one is complete. Bytes may arrive in any chunking.
class GrpcFrameReader {
public:
  void feed(std::string_view bytes, const GrpcFrameHandler &onFrame);

  /// True if trailing bytes remain that do not form a whole frame.
  bool truncated() const { return !buffer.empty(); }
  const std::string &error() const { return failure; }

private:
  std::string buffer;
  std::string failure;
};

std::string encodeFrame(std::string_view message);

class GrpcChannel {
public:
  /// @param endpoint "host:port"; @param clusterName sent as the `cluster_name`
  /// header the QOP gateway uses to route, omitted when empty.
  explicit GrpcChannel(std::string endpoint, std::string clusterName = "");

  const std::string &endpoint() const { return address; }
  void setTimeout(long seconds) { timeoutSeconds = seconds; }
  void setStreamTimeout(long seconds) { streamTimeoutSeconds = seconds; }
  void setVerbose(bool v) { verbose = v; }

  /// True if the linked libcurl advertises CURL_VERSION_HTTP2. Without it no
  /// call can succeed, so callers should check once and fail loudly.
  static bool http2Available();
  static std::string curlVersionSummary();

  GrpcResult invoke(std::string_view method, const std::string &request,
                    std::string &response);

  GrpcResult invokeStreaming(std::string_view method,
                             const std::string &request,
                             const GrpcFrameHandler &onFrame);

private:
  GrpcResult perform(std::string_view method, const std::string &request,
                     const GrpcFrameHandler &onFrame, long timeout);

  std::string address;
  std::string cluster;
  long timeoutSeconds = 30;
  long streamTimeoutSeconds = 0;
  bool verbose = false;
};

} // namespace cudaq::qm
