#include <mediaserver/devices/brompton_tessera.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace mediaserver::devices::tessera {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1U);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1U);
  }
  return text;
}

[[nodiscard]] std::string lower(std::string_view text) {
  std::string result{text};
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return result;
}

[[nodiscard]] std::string_view last_segment(std::string_view path) noexcept {
  while (!path.empty() && path.back() == '/') {
    path.remove_suffix(1U);
  }
  const std::size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1U);
}

[[nodiscard]] core::u32 to_u32(double value) noexcept {
  if (!(value > 0.0)) {
    return 0U;
  }
  if (value >= static_cast<double>(std::numeric_limits<core::u32>::max())) {
    return std::numeric_limits<core::u32>::max();
  }
  return static_cast<core::u32>(std::llround(value));
}

// What one endpoint answered, and whether an HTTP answer arrived at all.
struct Read final {
  std::optional<JsonValue> value;
  bool answered{};
  int status{};
  std::string error;
};

[[nodiscard]] Read read(const HttpEndpoint &endpoint, std::string_view path,
                        std::chrono::milliseconds timeout) {
  HttpRequest request{};
  request.endpoint = endpoint;
  request.path = std::string{path};
  request.timeout = timeout;
  request.max_response_bytes = 256U * 1024U;
  const HttpResult result = http_request(request);
  Read answer{};
  if (!result.response.has_value()) {
    answer.error = result.error;
    return answer;
  }
  answer.answered = true;
  answer.status = result.response->status;
  if (result.response->status != 200) {
    answer.error = "HTTP " + std::to_string(result.response->status);
    return answer;
  }
  answer.value = endpoint_value(result.response->body, path);
  if (!answer.value.has_value()) {
    answer.error = "unreadable answer";
  }
  return answer;
}

} // namespace

std::optional<JsonValue> endpoint_value(std::string_view body,
                                        std::string_view path) {
  const std::string key = lower(last_segment(path));
  if (const std::optional<JsonValue> parsed = parse_json(body)) {
    if (!parsed->is_object()) {
      return parsed;
    }
    // Keys are case-insensitive, as every Tessera path and tag is.
    const auto keys = parsed->keys();
    const auto items = parsed->items();
    for (std::size_t index = 0U; index < keys.size(); ++index) {
      if (lower(keys[index]) == key) {
        return items[index];
      }
    }
    if (items.size() == 1U) {
      return items.front();
    }
    return std::nullopt;
  }
  // {refresh-rate: 60}, as the manual prints it.
  std::string_view text = trim(body);
  if (text.size() < 2U || text.front() != '{' || text.back() != '}') {
    return std::nullopt;
  }
  text = trim(text.substr(1U, text.size() - 2U));
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view name = trim(text.substr(0U, colon));
  if (name.size() >= 2U && name.front() == '"' && name.back() == '"') {
    name = name.substr(1U, name.size() - 2U);
  }
  if (lower(name) != key) {
    return std::nullopt;
  }
  const std::string_view value = trim(text.substr(colon + 1U));
  if (std::optional<JsonValue> parsed = parse_json(value)) {
    return parsed;
  }
  // An unquoted enum or string.
  if (value.empty() || value.find_first_of("{}[],:\"") != std::string_view::npos) {
    return std::nullopt;
  }
  return JsonValue::string(std::string{value});
}

std::string model_name(std::string_view processor_type) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 6U>
      models{{{"m2", "M2"},
              {"s4", "S4"},
              {"s8", "S8"},
              {"t1", "T1"},
              {"t8", "T8"},
              {"sx40", "SX40"}}};
  const std::string type = lower(trim(processor_type));
  for (const auto &[key, name] : models) {
    if (type == key) {
      return std::string{name};
    }
  }
  return {};
}

std::string input_name(std::string_view port_type, core::u32 port_number) {
  std::string name{trim(port_type)};
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  if (name.empty()) {
    name = "Input";
  }
  if (port_number != 0U) {
    name += ' ' + std::to_string(port_number);
  }
  return name;
}

Poll poll(const HttpEndpoint &endpoint, std::chrono::milliseconds timeout) {
  Poll result{};
  topology::HardwareProcessorNode &node = result.node;
  node.reachability = topology::HardwareRuntimeReachability::Offline;
  node.all_inputs = false;

  const Read type = read(endpoint, processor_type_path, timeout);
  if (!type.answered) {
    node.detail = type.error.empty() ? "no answer" : type.error;
    return result;
  }
  result.reached = true;
  const std::string model =
      type.value.has_value() ? model_name(type.value->as_string()) : std::string{};
  if (model.empty()) {
    // Something answers on port 80, but not the Tessera API: IP control is
    // off, or it is not a Tessera processor.
    node.reachability =
        topology::HardwareRuntimeReachability::ConfigurationError;
    node.detail = type.status == 404
                      ? "no IP control - switch it on in the processor's "
                        "Live Control tile"
                      : "not a Tessera processor (" +
                            (type.error.empty() ? std::string{"unknown type"}
                                                : type.error) +
                            ")";
    return result;
  }
  node.reachability = topology::HardwareRuntimeReachability::Online;
  node.model = model;
  node.name = "Brompton " + model;

  const auto value = [&](std::string_view path) -> std::optional<JsonValue> {
    return read(endpoint, path, timeout).value;
  };
  const auto number = [&](std::string_view path) {
    const std::optional<JsonValue> found = value(path);
    return found.has_value() ? found->as_number(0.0) : 0.0;
  };
  const auto flag = [&](std::string_view path) {
    const std::optional<JsonValue> found = value(path);
    return found.has_value() && found->as_bool(false);
  };
  const auto text = [&](std::string_view path) {
    const std::optional<JsonValue> found = value(path);
    return found.has_value() ? std::string{found->as_string()} : std::string{};
  };

  node.software = text("/api/system/software-version");

  // The one input in use, as VIDEO IN 1.
  topology::HardwareProcessorInput input{};
  input.number = 1U;
  const core::u32 port = to_u32(number("/api/input/active/source/port-number"));
  const std::string port_type = text("/api/input/active/source/port-type");
  input.vendor_id = static_cast<core::i64>(port);
  input.name = input_name(port_type, port);
  input.connector = input_name(port_type, 0U);
  input.width = to_u32(number("/api/input/active/resolution/width"));
  input.height = to_u32(number("/api/input/active/resolution/height"));
  const double rate = number("/api/input/active/refresh-rate");
  input.refresh_millihertz =
      rate > 0.0 && rate < 1000.0 ? static_cast<core::u32>(std::llround(rate * 1000.0))
                                  : 0U;
  input.signal = input.width != 0U && input.height != 0U &&
                 input.refresh_millihertz != 0U;
  if (!input.signal) {
    input.width = 0U;
    input.height = 0U;
    input.refresh_millihertz = 0U;
  }
  node.inputs.push_back(std::move(input));

  node.panels = to_u32(number("/api/panels/statistics/associated-count"));
  node.panels_online = to_u32(number("/api/panels/statistics/online-count"));
  node.panels_faulted = to_u32(number("/api/panels/statistics/error-count"));
  node.blackout = flag("/api/override/blackout/enabled");
  node.frozen = flag("/api/override/freeze/enabled");
  node.test_pattern = flag("/api/override/test-pattern/enabled");
  node.preset = text("/api/presets/active/name");
  return result;
}

std::optional<topology::HardwareDiscoveredProcessor>
identify(const HttpEndpoint &endpoint, std::chrono::milliseconds timeout) {
  const Read type = read(endpoint, processor_type_path, timeout);
  if (!type.value.has_value()) {
    return std::nullopt;
  }
  const std::string model = model_name(type.value->as_string());
  if (model.empty()) {
    return std::nullopt;
  }
  topology::HardwareDiscoveredProcessor found{};
  found.protocol = topology::HardwareProcessorProtocol::BromptonTessera;
  found.model = model;
  found.name = "Brompton " + model;
  found.address = endpoint.port == default_port
                      ? endpoint.host
                      : endpoint.host + ':' + std::to_string(endpoint.port);
  return found;
}

} // namespace mediaserver::devices::tessera
