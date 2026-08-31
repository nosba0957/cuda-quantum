/****************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.               *
 * All rights reserved.                                                     *
 *                                                                          *
 * This source code and the accompanying materials are made available under *
 * the terms of the Apache License 2.0 which accompanies this distribution.  *
 ****************************************************************************/

// Implementation of the libcurl-backed gRPC client. See GrpcCurl.h.

#include "GrpcCurl.h"

#include <cctype>
#include <curl/curl.h>
#include <mutex>

namespace cudaq::qm {

namespace {

void globalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CallState {
  GrpcFrameReader reader;
  const GrpcFrameHandler *onFrame = nullptr;
  std::map<std::string, std::string> headers;
};

std::size_t onBody(char *ptr, std::size_t size, std::size_t nmemb,
                   void *userdata) {
  auto *state = static_cast<CallState *>(userdata);
  const std::size_t n = size * nmemb;
  state->reader.feed(std::string_view(ptr, n), *state->onFrame);
  return n;
}

// libcurl surfaces HTTP/2 trailers through the header callback as well, which
// is the only way grpc-status becomes visible without a gRPC library. The same
// callback therefore sees both the trailers-only error shape (grpc-status in
// the header block, empty body) and the normal one.
std::size_t onHeader(char *ptr, std::size_t size, std::size_t nmemb,
                     void *userdata) {
  auto *state = static_cast<CallState *>(userdata);
  const std::size_t n = size * nmemb;
  std::string line(ptr, n);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();
  const auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    for (auto &c : key)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    state->headers[key] = value;
  }
  return n;
}

int parseStatus(const std::map<std::string, std::string> &headers) {
  const auto it = headers.find("grpc-status");
  if (it == headers.end())
    return -1;
  try {
    return std::stoi(it->second);
  } catch (...) {
    return -1;
  }
}

} // namespace

std::string GrpcResult::describe() const {
  if (!transportError.empty())
    return "transport: " + transportError;
  std::string out = "grpc-status " + std::to_string(status);
  if (!message.empty())
    out += " (" + message + ")";
  out += ", HTTP " + std::to_string(httpStatus);
  return out;
}

std::string encodeFrame(std::string_view message) {
  std::string out;
  out.reserve(message.size() + 5);
  out.push_back('\0'); // compression flag: none
  const auto n = static_cast<std::uint32_t>(message.size());
  out.push_back(static_cast<char>((n >> 24) & 0xff));
  out.push_back(static_cast<char>((n >> 16) & 0xff));
  out.push_back(static_cast<char>((n >> 8) & 0xff));
  out.push_back(static_cast<char>(n & 0xff));
  out.append(message);
  return out;
}

void GrpcFrameReader::feed(std::string_view bytes,
                           const GrpcFrameHandler &onFrame) {
  if (!failure.empty())
    return;
  buffer.append(bytes);
  std::size_t offset = 0;
  while (buffer.size() - offset >= 5) {
    const auto *p =
        reinterpret_cast<const unsigned char *>(buffer.data()) + offset;
    if (p[0] != 0) {
      failure = "compressed gRPC frame, not supported";
      buffer.clear();
      return;
    }
    const std::uint32_t length = (static_cast<std::uint32_t>(p[1]) << 24) |
                                 (static_cast<std::uint32_t>(p[2]) << 16) |
                                 (static_cast<std::uint32_t>(p[3]) << 8) |
                                 static_cast<std::uint32_t>(p[4]);
    if (buffer.size() - offset - 5 < length)
      break;
    onFrame(std::string_view(buffer.data() + offset + 5, length));
    offset += 5 + length;
  }
  buffer.erase(0, offset);
}

GrpcChannel::GrpcChannel(std::string endpoint, std::string clusterName)
    : address(std::move(endpoint)), cluster(std::move(clusterName)) {
  globalInit();
}

bool GrpcChannel::http2Available() {
  const auto *info = curl_version_info(CURLVERSION_NOW);
  return (info->features & CURL_VERSION_HTTP2) != 0;
}

std::string GrpcChannel::curlVersionSummary() {
  const auto *info = curl_version_info(CURLVERSION_NOW);
  return std::string("libcurl ") + info->version +
         "  HTTP2=" + (http2Available() ? "yes" : "NO") + "  nghttp2=" +
         (info->nghttp2_version ? info->nghttp2_version : "none");
}

GrpcResult GrpcChannel::perform(std::string_view method,
                                const std::string &request,
                                const GrpcFrameHandler &onFrame, long timeout) {
  GrpcResult result;
  if (!http2Available()) {
    result.transportError =
        "libcurl built without HTTP/2 (" + curlVersionSummary() + ")";
    return result;
  }

  const std::string url = "http://" + address + std::string(method);
  const std::string body = encodeFrame(request);

  CURL *curl = curl_easy_init();
  if (!curl) {
    result.transportError = "curl_easy_init failed";
    return result;
  }

  CallState state;
  state.onFrame = &onFrame;

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "content-type: application/grpc+proto");
  headers = curl_slist_append(headers, "te: trailers");
  headers = curl_slist_append(headers, "x-grpc-service: gateway");
  headers =
      curl_slist_append(headers, "user-agent: cudaq-quantum-machines/0.1");
  headers = curl_slist_append(headers, "Expect:");
  if (!cluster.empty())
    headers = curl_slist_append(headers, ("cluster_name: " + cluster).c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                   CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
  if (verbose)
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    result.transportError = curl_easy_strerror(rc);
    return result;
  }
  if (!state.reader.error().empty()) {
    result.transportError = state.reader.error();
    return result;
  }

  result.status = parseStatus(state.headers);
  const auto msg = state.headers.find("grpc-message");
  if (msg != state.headers.end())
    result.message = msg->second;

  if (result.status == -1)
    result.transportError = "no grpc-status in headers or trailers";
  else if (result.status == 0 && state.reader.truncated())
    result.transportError = "response ended mid-frame";
  return result;
}

GrpcResult GrpcChannel::invoke(std::string_view method,
                               const std::string &request,
                               std::string &response) {
  int frames = 0;
  GrpcFrameHandler capture = [&](std::string_view frame) {
    if (frames++ == 0)
      response.assign(frame);
  };
  GrpcResult result = perform(method, request, capture, timeoutSeconds);
  if (result.ok() && frames != 1)
    result.transportError =
        "expected one response frame, got " + std::to_string(frames);
  return result;
}

GrpcResult GrpcChannel::invokeStreaming(std::string_view method,
                                        const std::string &request,
                                        const GrpcFrameHandler &onFrame) {
  return perform(method, request, onFrame, streamTimeoutSeconds);
}

} // namespace cudaq::qm
