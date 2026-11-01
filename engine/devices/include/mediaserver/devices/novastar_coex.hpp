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
#include <vector>

// NovaStar COEX controllers (MX40 Pro, MX30, MX20, KU20, CX40 Pro, CX80 Pro)
// through their HTTP API.
//
// The API answers JSON on port 8001 as {"code": 0, "data": ..., "message":
// "Success"}; some firmware answers a few endpoints with the bare value, and
// both are read. Only what the Director needs is asked for: the input
// sources (each with what arrives and the mode its EDID offers), the screens
// (raster, cabinets, and which input pixels land where), and the
// controller's name. Writing an input's EDID is the one change it makes, and
// only when the operator asks.
//
// Sources: NovaStar "COEX Series Interface API User Manual" and the online
// reference at api.coex.novastar.tech (GET /api/v1/screen). Field names are
// kept exactly as the controller sends them.
namespace mediaserver::devices::coex {

inline constexpr std::uint16_t default_port = 8001U;

inline constexpr std::string_view screen_path = "/api/v1/screen";
inline constexpr std::string_view screen_with_cabinets_path =
    "/api/v1/screen?isNeedCabinetInfo=true";
inline constexpr std::string_view input_sources_path =
    "/api/v1/device/input/sources";
inline constexpr std::string_view monitor_path = "/api/v1/device/monitor/info";
inline constexpr std::string_view hardware_path = "/api/v1/device/hw";

// The input connector types of the input source information.
enum class InputType : core::u8 {
  Dvi = 0x00U,
  DualDvi = 0x01U,
  Hdmi14 = 0x02U,
  Hdmi20 = 0x03U,
  Dp11 = 0x04U,
  Dp12 = 0x05U,
  Dp14 = 0x06U,
  Sdi3G = 0x07U,
  Sdi6G = 0x08U,
  Sdi12G = 0x09U,
  PipVideo = 0x0AU,
  Hdmi13 = 0x10U,
  Hdmi21 = 0x11U,
  Pcie = 0x12U,
  Serdes = 0x13U,
  Lvds = 0x14U,
  Internal = 0xE0U,
  None = 0xFFU,
};

// "HDMI 2.0", "DP 1.2", "12G-SDI" ...; empty for an unknown type.
[[nodiscard]] std::string_view connector_name(core::u32 type) noexcept;

// The value of an answer: its "data" member when it is wrapped, otherwise
// the answer itself. A wrapped answer with a non-zero code is an error and
// gives its message instead.
struct Unwrapped final {
  const JsonValue *data{};
  std::string error;
};
[[nodiscard]] Unwrapped unwrap(const JsonValue &answer);

// One input source as the controller lists it.
struct Input final {
  core::i64 id{};
  core::u32 type{0xFFU};
  core::i64 group_id{-1};
  std::string name;
  bool signal{};
  core::u32 width{};
  core::u32 height{};
  core::u32 refresh_millihertz{};
  core::u32 edid_width{};
  core::u32 edid_height{};
  core::u32 edid_refresh_millihertz{};
  bool edid_supported{};

  bool operator==(const Input &) const = default;
};

// Every input in the answer's order. Internal test sources and slots with
// no connector are kept here and left off the map later.
[[nodiscard]] std::vector<Input> parse_inputs(const JsonValue &data);

// The inputs a card shows as VIDEO IN 1, 2, ...: real connectors, in the
// controller's order.
[[nodiscard]] std::vector<topology::HardwareProcessorInput>
map_inputs(const std::vector<Input> &inputs);

// The screens of GET /api/v1/screen. A layer's source names an input by its
// group id; `inputs` turns that into the VIDEO IN number.
[[nodiscard]] std::vector<topology::HardwareProcessorScreen>
parse_screens(const JsonValue &data, const std::vector<Input> &inputs);

// The controller's name for itself, from the monitoring answer, and its
// model when the name carries one ("MX40 Pro_002198" is an MX40 Pro).
struct Identity final {
  std::string name;
  std::string model;
};
[[nodiscard]] Identity parse_identity(const JsonValue *monitor,
                                      const JsonValue *hardware);

// The request body that sets an input's EDID to one mode. Both shapes the
// API has used are sent - the manual's top-level resolution and the newer
// "para" envelope - since the controller ignores members it does not know.
[[nodiscard]] std::string edid_request_body(core::u32 width, core::u32 height,
                                            core::u32 refresh_millihertz);
[[nodiscard]] std::string edid_path(core::i64 input_id);

// One complete read of a controller: everything a processor card shows.
struct Poll final {
  topology::HardwareProcessorNode node;
  // The controller's own input list, kept to address EDID writes.
  std::vector<Input> inputs;
  // The controller answered at all.
  bool reached{};
};

[[nodiscard]] Poll poll(const HttpEndpoint &endpoint,
                        std::chrono::milliseconds timeout);

// Whether something on that endpoint is a COEX controller, and its name.
[[nodiscard]] std::optional<topology::HardwareDiscoveredProcessor>
identify(const HttpEndpoint &endpoint, std::chrono::milliseconds timeout);

struct WriteResult final {
  bool ok{};
  std::string message;
};

[[nodiscard]] WriteResult write_edid(const HttpEndpoint &endpoint,
                                     core::i64 input_id, core::u32 width,
                                     core::u32 height,
                                     core::u32 refresh_millihertz,
                                     std::chrono::milliseconds timeout);

} // namespace mediaserver::devices::coex
