/****************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.               *
 * All rights reserved.                                                     *
 *                                                                          *
 * This source code and the accompanying materials are made available under *
 * the terms of the Apache License 2.0 which accompanies this distribution.  *
 ****************************************************************************/

// P0 transport spike: one unary gRPC call (QmmService/GetVersion) over
// cleartext HTTP/2 using libcurl prior-knowledge, no grpc++ linked.
//
//   build: see tests/README or run_spike.sh
//   run:   ./grpc_curl_spike [host] [port] [cluster_name]

#include "qm_min.pb.h"
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <map>
#include <string>

namespace {

struct Response {
  std::string body;
  std::map<std::string, std::string> headers;
  long status = 0;
};

size_t onBody(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *r = static_cast<Response *>(userdata);
  r->body.append(ptr, size * nmemb);
  return size * nmemb;
}

// libcurl routes HTTP/2 trailers through the header callback as well, which is
// the only way grpc-status becomes visible without a gRPC library.
size_t onHeader(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *r = static_cast<Response *>(userdata);
  std::string line(ptr, size * nmemb);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();
  auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    val.erase(0, val.find_first_not_of(" \t"));
    for (auto &c : key)
      c = static_cast<char>(tolower(c));
    r->headers[key] = val;
    fprintf(stderr, "  [hdr] %s: %s\n", key.c_str(), val.c_str());
  } else if (!line.empty()) {
    fprintf(stderr, "  [sts] %s\n", line.c_str());
  }
  return size * nmemb;
}

std::string frame(const std::string &msg) {
  std::string out;
  out.push_back('\0'); // no compression
  uint32_t n = static_cast<uint32_t>(msg.size());
  out.push_back(static_cast<char>((n >> 24) & 0xff));
  out.push_back(static_cast<char>((n >> 16) & 0xff));
  out.push_back(static_cast<char>((n >> 8) & 0xff));
  out.push_back(static_cast<char>(n & 0xff));
  out.append(msg);
  return out;
}

bool unframe(const std::string &body, std::string &msg, std::string &err) {
  if (body.size() < 5) {
    err = "response shorter than a gRPC frame header (" +
          std::to_string(body.size()) + " bytes)";
    return false;
  }
  if (body[0] != 0) {
    err = "compressed response frame, not handled by the spike";
    return false;
  }
  uint32_t n = (static_cast<uint8_t>(body[1]) << 24) |
               (static_cast<uint8_t>(body[2]) << 16) |
               (static_cast<uint8_t>(body[3]) << 8) |
               static_cast<uint8_t>(body[4]);
  if (body.size() < 5 + n) {
    err = "truncated frame: header says " + std::to_string(n) + ", have " +
          std::to_string(body.size() - 5);
    return false;
  }
  msg = body.substr(5, n);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const std::string host = argc > 1 ? argv[1] : "10.21.19.201";
  const std::string port = argc > 2 ? argv[2] : "9514";
  const std::string cluster = argc > 3 ? argv[3] : "QPX1000_4";

  const auto *info = curl_version_info(CURLVERSION_NOW);
  printf("libcurl %s  HTTP2=%s  nghttp2=%s\n", info->version,
         (info->features & CURL_VERSION_HTTP2) ? "yes" : "NO",
         info->nghttp2_version ? info->nghttp2_version : "none");
  if (!(info->features & CURL_VERSION_HTTP2)) {
    fprintf(stderr, "FAIL: libcurl has no HTTP/2 support\n");
    return 2;
  }

  qm::grpc::v2::GetVersionRequest req;
  std::string payload;
  req.SerializeToString(&payload);
  const std::string body = frame(payload);

  const std::string url =
      "http://" + host + ":" + port + "/qm.grpc.v2.QmmService/GetVersion";
  printf("POST %s  (%zu byte frame)\n", url.c_str(), body.size());

  curl_global_init(CURL_GLOBAL_DEFAULT);
  CURL *curl = curl_easy_init();
  Response resp;

  curl_slist *hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "content-type: application/grpc+proto");
  hdrs = curl_slist_append(hdrs, "te: trailers");
  hdrs = curl_slist_append(hdrs, "x-grpc-service: gateway");
  hdrs = curl_slist_append(hdrs, ("cluster_name: " + cluster).c_str());
  hdrs = curl_slist_append(hdrs, "user-agent: cudaq-qm-spike/0.1");
  hdrs = curl_slist_append(hdrs, "Expect:"); // curl would otherwise add it

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                   CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
  if (getenv("SPIKE_VERBOSE"))
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

  fprintf(stderr, "--- headers/trailers seen ---\n");
  CURLcode rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
  long httpVersion = 0;
  curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &httpVersion);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  curl_global_cleanup();

  if (rc != CURLE_OK) {
    fprintf(stderr, "FAIL: curl_easy_perform: %s\n", curl_easy_strerror(rc));
    return 3;
  }
  printf("HTTP %ld  (CURLINFO_HTTP_VERSION %ld, 3 == HTTP/2)  body %zu bytes\n",
         resp.status, httpVersion, resp.body.size());

  auto statusIt = resp.headers.find("grpc-status");
  if (statusIt == resp.headers.end()) {
    fprintf(stderr,
            "WARN: no grpc-status header/trailer surfaced by libcurl\n");
  } else {
    printf("grpc-status: %s  grpc-message: %s\n", statusIt->second.c_str(),
           resp.headers.count("grpc-message")
               ? resp.headers["grpc-message"].c_str()
               : "");
    if (statusIt->second != "0")
      return 4;
  }

  std::string msg, err;
  if (!unframe(resp.body, msg, err)) {
    fprintf(stderr, "FAIL: %s\n", err.c_str());
    return 5;
  }

  qm::grpc::v2::GetVersionResponse out;
  if (!out.ParseFromString(msg)) {
    fprintf(stderr, "FAIL: GetVersionResponse did not parse (%zu bytes)\n",
            msg.size());
    return 6;
  }
  if (out.has_error()) {
    fprintf(stderr, "FAIL: server error: %s\n", out.error().details().c_str());
    return 7;
  }
  printf("\nQOP gateway version: %s\n", out.success().gateway().c_str());
  for (const auto &kv : out.success().controllers())
    printf("controller %s: %s\n", kv.first.c_str(), kv.second.c_str());
  return 0;
}
