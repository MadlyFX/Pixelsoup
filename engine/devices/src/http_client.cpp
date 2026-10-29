#include <mediaserver/devices/http_client.hpp>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <system_error>
#include <utility>

namespace mediaserver::devices {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] bool equals_ignoring_case(std::string_view left,
                                        std::string_view right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](char a, char b) {
                      return std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b));
                    });
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1U);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                           text.back() == '\r')) {
    text.remove_suffix(1U);
  }
  return text;
}

struct Head final {
  int status{};
  std::optional<std::size_t> content_length;
  bool chunked{};
  std::size_t body_offset{};
};

// The status line and the headers this client acts on, once the blank line
// that ends them has arrived.
[[nodiscard]] std::optional<Head> parse_head(std::string_view received) {
  const std::size_t end = received.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  Head head{};
  head.body_offset = end + 4U;
  std::string_view lines = received.substr(0U, end);
  const std::size_t first_break = lines.find("\r\n");
  const std::string_view status_line = lines.substr(0U, first_break);
  // "HTTP/1.1 200 OK"
  const std::size_t space = status_line.find(' ');
  if (status_line.substr(0U, 5U) != "HTTP/" || space == std::string_view::npos) {
    head.status = -1;
    return head;
  }
  const std::string_view code = status_line.substr(space + 1U, 3U);
  if (std::from_chars(code.data(), code.data() + code.size(), head.status).ec !=
      std::errc{}) {
    head.status = -1;
    return head;
  }
  lines = first_break == std::string_view::npos ? std::string_view{}
                                                : lines.substr(first_break + 2U);
  while (!lines.empty()) {
    const std::size_t next = lines.find("\r\n");
    const std::string_view line = lines.substr(0U, next);
    lines = next == std::string_view::npos ? std::string_view{}
                                           : lines.substr(next + 2U);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    const std::string_view name = trim(line.substr(0U, colon));
    const std::string_view value = trim(line.substr(colon + 1U));
    if (equals_ignoring_case(name, "content-length")) {
      std::size_t length{};
      if (std::from_chars(value.data(), value.data() + value.size(), length)
              .ec == std::errc{}) {
        head.content_length = length;
      }
    } else if (equals_ignoring_case(name, "transfer-encoding")) {
      std::string lowered{value};
      std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                     [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                     });
      head.chunked = lowered.find("chunked") != std::string::npos;
    }
  }
  return head;
}

[[nodiscard]] std::string describe(const std::error_code &error) {
  if (error == asio::error::connection_refused) {
    return "connection refused";
  }
  if (error == asio::error::host_unreachable ||
      error == asio::error::network_unreachable) {
    return "unreachable";
  }
  if (error == asio::error::connection_reset) {
    return "connection reset";
  }
  return error.message();
}

} // namespace

std::optional<HttpEndpoint> parse_http_endpoint(std::string_view address,
                                                std::uint16_t default_port) {
  address = trim(address);
  if (address.empty()) {
    return std::nullopt;
  }
  std::string_view host = address;
  std::string_view port_text;
  if (address.front() == '[') {
    const std::size_t close = address.find(']');
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    host = address.substr(1U, close - 1U);
    const std::string_view rest = address.substr(close + 1U);
    if (!rest.empty()) {
      if (rest.front() != ':') {
        return std::nullopt;
      }
      port_text = rest.substr(1U);
    }
  } else if (std::count(address.begin(), address.end(), ':') == 1) {
    const std::size_t colon = address.find(':');
    host = address.substr(0U, colon);
    port_text = address.substr(colon + 1U);
  }
  std::error_code error;
  const asio::ip::address parsed = asio::ip::make_address(std::string{host}, error);
  if (error) {
    return std::nullopt;
  }
  std::uint16_t port = default_port;
  if (!port_text.empty()) {
    unsigned value{};
    const auto result = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), value);
    if (result.ec != std::errc{} ||
        result.ptr != port_text.data() + port_text.size() || value == 0U ||
        value > 65'535U) {
      return std::nullopt;
    }
    port = static_cast<std::uint16_t>(value);
  }
  return HttpEndpoint{parsed.to_string(), port};
}

std::optional<std::string> decode_chunked_body(std::string_view encoded,
                                               std::size_t max_bytes) {
  std::string body;
  while (true) {
    const std::size_t line_end = encoded.find("\r\n");
    if (line_end == std::string_view::npos) {
      return std::nullopt;
    }
    std::string_view size_text = encoded.substr(0U, line_end);
    // Chunk extensions follow a semicolon and are ignored.
    size_text = trim(size_text.substr(0U, size_text.find(';')));
    std::size_t size{};
    const auto parsed = std::from_chars(
        size_text.data(), size_text.data() + size_text.size(), size, 16);
    if (size_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != size_text.data() + size_text.size()) {
      return std::nullopt;
    }
    encoded.remove_prefix(line_end + 2U);
    if (size == 0U) {
      return body;
    }
    if (size > max_bytes || body.size() > max_bytes - size ||
        encoded.size() < size + 2U || encoded.substr(size, 2U) != "\r\n") {
      return std::nullopt;
    }
    body.append(encoded.substr(0U, size));
    encoded.remove_prefix(size + 2U);
  }
}

HttpResult http_request(const HttpRequest &request) {
  HttpResult result{};
  std::error_code error;
  const asio::ip::address address =
      asio::ip::make_address(request.endpoint.host, error);
  if (error) {
    result.error = "not a numeric address";
    return result;
  }
  const Clock::time_point deadline = Clock::now() + request.timeout;
  asio::io_context io;
  asio::ip::tcp::socket socket{io};

  // Runs the context until `done` or the deadline. A deadline closes the
  // socket, which completes the pending operation with an error.
  const auto wait = [&](const bool &done) {
    io.restart();
    while (!done) {
      const Clock::time_point now = Clock::now();
      if (now >= deadline) {
        std::error_code ignored;
        socket.close(ignored);
        io.restart();
        io.run_for(std::chrono::milliseconds{50});
        return false;
      }
      io.run_for(std::chrono::duration_cast<std::chrono::milliseconds>(
                     deadline - now) +
                 std::chrono::milliseconds{1});
      if (io.stopped()) {
        io.restart();
      }
    }
    return true;
  };

  bool connected{};
  std::error_code connect_error;
  socket.async_connect(
      asio::ip::tcp::endpoint{address, request.endpoint.port},
      [&](const std::error_code &failure) {
        connect_error = failure;
        connected = true;
      });
  if (!wait(connected)) {
    result.error = "timed out";
    return result;
  }
  if (connect_error) {
    result.error = describe(connect_error);
    return result;
  }

  std::string head = request.method + ' ' + request.path + " HTTP/1.1\r\n";
  std::string host = request.endpoint.host;
  if (address.is_v6()) {
    host = '[' + host + ']';
  }
  head += "Host: " + host + ':' + std::to_string(request.endpoint.port) + "\r\n";
  head += "Accept: application/json\r\nConnection: close\r\n";
  head += "User-Agent: mediaserver\r\n";
  if (!request.body.empty() || request.method == "PUT" ||
      request.method == "POST") {
    head += "Content-Type: application/json\r\n";
    head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  }
  head += "\r\n";
  head += request.body;

  bool written{};
  std::error_code write_error;
  asio::async_write(socket, asio::buffer(head),
                    [&](const std::error_code &failure, std::size_t) {
                      write_error = failure;
                      written = true;
                    });
  if (!wait(written)) {
    result.error = "timed out";
    return result;
  }
  if (write_error) {
    result.error = describe(write_error);
    return result;
  }

  std::string received;
  std::array<char, 16U * 1024U> chunk{};
  std::optional<Head> parsed_head;
  while (true) {
    bool read_done{};
    std::error_code read_error;
    std::size_t read_bytes{};
    socket.async_read_some(asio::buffer(chunk),
                           [&](const std::error_code &failure, std::size_t n) {
                             read_error = failure;
                             read_bytes = n;
                             read_done = true;
                           });
    if (!wait(read_done)) {
      result.error = "timed out";
      return result;
    }
    received.append(chunk.data(), read_bytes);
    if (received.size() > request.max_response_bytes + 64U * 1024U) {
      result.error = "answer too large";
      return result;
    }
    const bool closed = read_error == asio::error::eof;
    if (read_error && !closed) {
      result.error = describe(read_error);
      return result;
    }
    if (!parsed_head.has_value()) {
      parsed_head = parse_head(received);
      if (parsed_head.has_value() && parsed_head->status < 0) {
        result.error = "not an HTTP answer";
        return result;
      }
    }
    if (parsed_head.has_value()) {
      const std::string_view body =
          std::string_view{received}.substr(parsed_head->body_offset);
      std::optional<std::string> complete;
      if (parsed_head->chunked) {
        // A chunked body is complete once its last chunk has arrived.
        if (body.find("0\r\n\r\n") != std::string_view::npos || closed) {
          complete = decode_chunked_body(body, request.max_response_bytes);
          if (!complete.has_value() && closed) {
            result.error = "malformed chunked answer";
            return result;
          }
        }
      } else if (parsed_head->content_length.has_value()) {
        if (*parsed_head->content_length > request.max_response_bytes) {
          result.error = "answer too large";
          return result;
        }
        if (body.size() >= *parsed_head->content_length) {
          complete = std::string{body.substr(0U, *parsed_head->content_length)};
        } else if (closed) {
          result.error = "answer cut short";
          return result;
        }
      } else if (closed) {
        complete = std::string{body};
      }
      if (complete.has_value()) {
        result.response = HttpResponse{parsed_head->status, std::move(*complete)};
        std::error_code ignored;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
        return result;
      }
    }
    if (closed) {
      result.error = "connection closed before an answer";
      return result;
    }
  }
}

} // namespace mediaserver::devices
