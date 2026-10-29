#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/devices/http_client.hpp>
#include <mediaserver/devices/json.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Brompton Tessera processors (M2, S4, S8, T1, T8, SX40) through their IP
// control API.
//
// Tessera 3.1 serves a filesystem-like tree under /api on HTTP port 80: a GET
// of an endpoint answers {"<last path segment>": value}. IP control must be
// switched on in the processor's Live Control tile. The tree reports the one
// input in use (its port, raster and rate), panel counts, the overrides that
// replace the picture, the active preset and the model - but no EDID and no
// screen layout, so a Tessera card is checked for mode and signal only and
// Build from hardware lays its windows side by side.
//
// Source: Brompton "Tessera Processing IP Control API", software 3.1.0
// beta 4. Paths are kept exactly as documented.
namespace mediaserver::devices::tessera {

inline constexpr std::uint16_t default_port = 80U;

inline constexpr std::string_view processor_type_path =
    "/api/system/processor-type";

// The value of an endpoint's answer: the member named after the path's last
// segment (or the only member), or a bare value. The manual prints answers
// with unquoted keys - {refresh-rate: 60} - so a single unquoted pair is
// read too.
[[nodiscard]] std::optional<JsonValue> endpoint_value(std::string_view body,
                                                      std::string_view path);

// "S8" for "s8", "SX40" for "sx40"; empty for anything else.
[[nodiscard]] std::string model_name(std::string_view processor_type);

// "HDMI 1", "SDI 2": the active input as its port reads.
[[nodiscard]] std::string input_name(std::string_view port_type,
                                     core::u32 port_number);

struct Poll final {
  topology::HardwareProcessorNode node;
  // The processor answered at all.
  bool reached{};
};

[[nodiscard]] Poll poll(const HttpEndpoint &endpoint,
                        std::chrono::milliseconds timeout);

// Whether something on that endpoint is a Tessera processor.
[[nodiscard]] std::optional<topology::HardwareDiscoveredProcessor>
identify(const HttpEndpoint &endpoint, std::chrono::milliseconds timeout);

} // namespace mediaserver::devices::tessera
