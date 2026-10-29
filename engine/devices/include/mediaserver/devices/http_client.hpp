#pragma once

#include <mediaserver/core/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// A minimal HTTP/1.1 client for device control APIs on the production LAN.
//
// One blocking request per call, on the caller's thread, with one deadline
// covering the connect, the request and the whole answer, so a device that
// stops answering costs at most that long. Hosts are numeric addresses only:
// a control address is never left to name resolution. Answers may carry a
// Content-Length, be chunked, or run to the close of the connection.
namespace mediaserver::devices {

struct HttpEndpoint final {
  // A numeric IPv4 or IPv6 address, without brackets.
  std::string host;
  std::uint16_t port{80U};

  bool operator==(const HttpEndpoint &) const = default;
};

// "10.0.0.40", "10.0.0.40:8001", "[fe80::1]:8001" or "::1". A missing port
// takes `default_port`. Anything that is not a numeric address is refused.
[[nodiscard]] std::optional<HttpEndpoint>
parse_http_endpoint(std::string_view address, std::uint16_t default_port);

struct HttpRequest final {
  std::string method{"GET"};
  HttpEndpoint endpoint;
  std::string path{"/"};
  // Sent with Content-Type: application/json when not empty.
  std::string body;
  std::chrono::milliseconds timeout{2'000};
  std::size_t max_response_bytes{8U * 1024U * 1024U};
};

struct HttpResponse final {
  int status{};
  std::string body;
};

struct HttpResult final {
  // Set when a complete answer arrived, whatever its status.
  std::optional<HttpResponse> response;
  // Why there is none: "timed out", "connection refused", ...
  std::string error;
};

[[nodiscard]] HttpResult http_request(const HttpRequest &request);

// Decodes a complete chunked body. Nothing for one that is malformed or
// unfinished.
[[nodiscard]] std::optional<std::string>
decode_chunked_body(std::string_view encoded, std::size_t max_bytes);

} // namespace mediaserver::devices
