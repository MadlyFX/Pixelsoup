#include <mediaserver/devices/novastar_coex.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace mediaserver::devices::coex {
namespace {

[[nodiscard]] core::u32 to_u32(double value) noexcept {
  if (!(value > 0.0)) {
    return 0U;
  }
  if (value >= static_cast<double>(std::numeric_limits<core::u32>::max())) {
    return std::numeric_limits<core::u32>::max();
  }
  return static_cast<core::u32>(std::llround(value));
}

[[nodiscard]] core::u32 to_millihertz(double hertz) noexcept {
  if (!(hertz > 0.0) || hertz > 10'000.0) {
    return 0U;
  }
  return static_cast<core::u32>(std::llround(hertz * 1000.0));
}

// {"width": w, "height": h} under `key`, or zeroes.
[[nodiscard]] std::pair<core::u32, core::u32> size_at(const JsonValue &object,
                                                      std::string_view key) {
  const JsonValue *const size = object.find(key);
  if (size == nullptr) {
    return {0U, 0U};
  }
  return {to_u32(size->number_at("width")), to_u32(size->number_at("height"))};
}

[[nodiscard]] std::pair<core::u32, core::u32>
position_at(const JsonValue &object, std::string_view key) {
  const JsonValue *const position = object.find(key);
  if (position == nullptr) {
    return {0U, 0U};
  }
  return {to_u32(position->number_at("x")), to_u32(position->number_at("y"))};
}

// The first non-zero {"width", "height"} under one of `keys`, or the
// object's own width and height. Firmware differs in where it puts a
// canvas's or cabinet's pixel size; "resolution" comes first because a
// cabinet's "size" may be its physical size where both are given.
[[nodiscard]] std::pair<core::u32, core::u32>
extent_of(const JsonValue &object,
          std::initializer_list<std::string_view> keys) {
  for (const std::string_view key : keys) {
    const auto extent = size_at(object, key);
    if (extent.first != 0U && extent.second != 0U) {
      return extent;
    }
  }
  return {to_u32(object.number_at("width")), to_u32(object.number_at("height"))};
}

// Its "position", or its own x and y.
[[nodiscard]] std::pair<core::u32, core::u32>
place_of(const JsonValue &object) {
  if (object.find("position") != nullptr) {
    return position_at(object, "position");
  }
  return {to_u32(object.number_at("x")), to_u32(object.number_at("y"))};
}

// The array an answer's data holds: the data itself, or the first array
// member of an object that wraps one.
[[nodiscard]] const JsonValue *list_in(const JsonValue &data) noexcept {
  if (data.is_array()) {
    return &data;
  }
  if (data.is_object()) {
    for (const JsonValue &member : data.items()) {
      if (member.is_array()) {
        return &member;
      }
    }
  }
  return nullptr;
}

[[nodiscard]] std::string text_of(const JsonValue *value) {
  if (value == nullptr) {
    return {};
  }
  if (value->kind() == JsonValue::Kind::String) {
    return std::string{value->as_string()};
  }
  if (value->kind() == JsonValue::Kind::Number) {
    return std::to_string(static_cast<long long>(value->as_number()));
  }
  return {};
}

[[nodiscard]] bool real_connector(core::u32 type) noexcept {
  return type != static_cast<core::u32>(InputType::Internal) &&
         type != static_cast<core::u32>(InputType::None) &&
         type != static_cast<core::u32>(InputType::PipVideo);
}

[[nodiscard]] bool standard_mode(core::u32 width, core::u32 height) noexcept {
  constexpr std::array<std::pair<core::u32, core::u32>, 10U> modes{{
      {1280U, 720U},
      {1920U, 1080U},
      {1920U, 1200U},
      {2048U, 1080U},
      {2560U, 1440U},
      {2560U, 1600U},
      {3840U, 2160U},
      {4096U, 2160U},
      {7680U, 4320U},
      {1600U, 1200U},
  }};
  return std::find(modes.begin(), modes.end(),
                   std::pair{width, height}) != modes.end();
}

struct Answer final {
  std::optional<JsonValue> json;
  // An HTTP answer arrived, whatever it said.
  bool answered{};
  std::string error;
};

[[nodiscard]] Answer get(const HttpEndpoint &endpoint, std::string_view path,
                         std::chrono::milliseconds timeout) {
  HttpRequest request{};
  request.endpoint = endpoint;
  request.path = std::string{path};
  request.timeout = timeout;
  const HttpResult result = http_request(request);
  Answer answer{};
  if (!result.response.has_value()) {
    answer.error = result.error;
    return answer;
  }
  answer.answered = true;
  if (result.response->status != 200) {
    answer.error = "HTTP " + std::to_string(result.response->status);
    return answer;
  }
  answer.json = parse_json(result.response->body);
  if (!answer.json.has_value()) {
    answer.error = "answer is not JSON";
  }
  return answer;
}

} // namespace

std::string_view connector_name(core::u32 type) noexcept {
  switch (type) {
  case 0x00U:
    return "DVI";
  case 0x01U:
    return "Dual DVI";
  case 0x02U:
    return "HDMI 1.4";
  case 0x03U:
    return "HDMI 2.0";
  case 0x04U:
    return "DP 1.1";
  case 0x05U:
    return "DP 1.2";
  case 0x06U:
    return "DP 1.4";
  case 0x07U:
    return "3G-SDI";
  case 0x08U:
    return "6G-SDI";
  case 0x09U:
    return "12G-SDI";
  case 0x0AU:
    return "PIP";
  case 0x10U:
    return "HDMI 1.3";
  case 0x11U:
    return "HDMI 2.1";
  case 0x12U:
    return "PCIe";
  case 0x13U:
    return "SerDes";
  case 0x14U:
    return "LVDS";
  case 0xE0U:
    return "Internal";
  default:
    return {};
  }
}

Unwrapped unwrap(const JsonValue &answer) {
  Unwrapped result{};
  if (answer.is_object() && answer.find("code") != nullptr) {
    const double code = answer.number_at("code", -1.0);
    if (code != 0.0) {
      result.error = std::string{answer.string_at("message")};
      if (result.error.empty()) {
        result.error = "error " + std::to_string(static_cast<long long>(code));
      }
      return result;
    }
    result.data = answer.find("data");
    return result;
  }
  result.data = &answer;
  return result;
}

std::vector<Input> parse_inputs(const JsonValue &data) {
  std::vector<Input> inputs;
  const JsonValue *const list = list_in(data);
  if (list == nullptr) {
    return inputs;
  }
  for (const JsonValue &source : list->items()) {
    if (!source.is_object()) {
      continue;
    }
    Input input{};
    input.id = static_cast<core::i64>(source.number_at("id", -1.0));
    input.type = static_cast<core::u32>(source.number_at("type", 255.0));
    input.group_id = static_cast<core::i64>(source.number_at("groupId", -1.0));
    input.name = std::string{source.string_at("name")};
    std::tie(input.width, input.height) = size_at(source, "actualResolution");
    input.refresh_millihertz =
        to_millihertz(source.number_at("actualRefreshRate"));
    if (const JsonValue *const edid = source.find("defaultEDID")) {
      std::tie(input.edid_width, input.edid_height) =
          size_at(*edid, "resolution");
      // A standard EDID names its preferred timing here (3840x2160 at 60)
      // but offers every rate in supportFrameRate, so only a custom EDID
      // pins the rate a source has to send.
      const bool custom = source.bool_at("isEdidCustom", false) ||
                          edid->bool_at("isCustom", false);
      input.edid_refresh_millihertz =
          custom ? to_millihertz(edid->number_at("refreshRate")) : 0U;
    }
    input.edid_supported = source.bool_at("isSupportEDID", false);
    // Firmware reports a signal as sourceStatus (or signalStatus); an input
    // with neither is taken as live when it reports a picture size.
    if (const JsonValue *const status = source.find("sourceStatus")) {
      input.signal = status->as_number() != 0.0;
    } else if (const JsonValue *const signal = source.find("signalStatus")) {
      input.signal = signal->as_number() != 0.0;
    } else {
      input.signal = input.width != 0U && input.height != 0U;
    }
    if (!input.signal) {
      // What a dark input reports is its EDID's default, not a picture.
      input.width = 0U;
      input.height = 0U;
      input.refresh_millihertz = 0U;
    }
    inputs.push_back(std::move(input));
  }
  return inputs;
}

std::vector<topology::HardwareProcessorInput>
map_inputs(const std::vector<Input> &inputs) {
  std::vector<topology::HardwareProcessorInput> mapped;
  core::u8 number = 1U;
  for (const Input &input : inputs) {
    if (!real_connector(input.type)) {
      continue;
    }
    if (number > topology::max_processor_video_inputs) {
      break;
    }
    topology::HardwareProcessorInput shown{};
    shown.number = number++;
    shown.vendor_id = input.id;
    shown.connector = std::string{connector_name(input.type)};
    shown.name = input.name.empty() ? shown.connector : input.name;
    shown.signal = input.signal;
    shown.width = input.width;
    shown.height = input.height;
    shown.refresh_millihertz = input.refresh_millihertz;
    shown.edid_width = input.edid_width;
    shown.edid_height = input.edid_height;
    shown.edid_refresh_millihertz = input.edid_refresh_millihertz;
    shown.edid_writable = input.edid_supported;
    mapped.push_back(std::move(shown));
  }
  return mapped;
}

std::vector<topology::HardwareProcessorScreen>
parse_screens(const JsonValue &data, const std::vector<Input> &inputs) {
  std::vector<topology::HardwareProcessorScreen> screens;
  const JsonValue *const list =
      data.find("screens") != nullptr ? data.find("screens") : list_in(data);
  if (list == nullptr || !list->is_array()) {
    return screens;
  }
  const std::vector<topology::HardwareProcessorInput> mapped =
      map_inputs(inputs);
  const auto input_for_group = [&](core::i64 group) -> const Input * {
    for (const Input &input : inputs) {
      if (input.group_id == group || (input.group_id < 0 && input.id == group)) {
        return &input;
      }
    }
    return nullptr;
  };
  const auto number_of = [&](const Input *input) -> core::u8 {
    if (input == nullptr) {
      return 0U;
    }
    for (const topology::HardwareProcessorInput &shown : mapped) {
      if (shown.vendor_id == input->id) {
        return shown.number;
      }
    }
    return 0U;
  };

  for (const JsonValue &entry : list->items()) {
    if (!entry.is_object()) {
      continue;
    }
    topology::HardwareProcessorScreen screen{};
    screen.id = text_of(entry.find("screenID"));
    screen.name = std::string{entry.string_at("screenName")};
    screen.refresh_millihertz = to_millihertz(entry.number_at("masterFrameRate"));
    const int working_mode =
        static_cast<int>(entry.number_at("workingMode", 1.0));

    // The raster is what the mapped cabinets cover. A cabinet sits within its
    // canvas, which sits within the screen; some firmware lists the cabinets
    // beside the canvases instead, in screen pixels. The canvases' and the
    // screen's declared sizes can be far larger than the panels (a 2880x1536
    // wall came back over 32768 square), so they only count when no cabinet
    // is listed. Canvas and layer positions are in the controller's layout
    // space, which VMP places anywhere (a 2880x1536 wall's canvas sat at
    // 156782,42555), so the screen starts at its top-left canvas.
    const auto far_edge = [](core::u32 a, core::u32 b) noexcept {
      return b > std::numeric_limits<core::u32>::max() - a
                 ? std::numeric_limits<core::u32>::max()
                 : a + b;
    };
    core::u32 mapped_width{};
    core::u32 mapped_height{};
    core::u32 declared_width{};
    core::u32 declared_height{};
    core::u32 origin_x{};
    core::u32 origin_y{};
    if (const JsonValue *const canvases = entry.find("canvases")) {
      bool first = true;
      for (const JsonValue &canvas : canvases->items()) {
        if (!canvas.is_object()) {
          continue;
        }
        const auto [canvas_x, canvas_y] = place_of(canvas);
        origin_x = first ? canvas_x : std::min(origin_x, canvas_x);
        origin_y = first ? canvas_y : std::min(origin_y, canvas_y);
        first = false;
      }
    }
    const auto add_cabinets = [&](const JsonValue *cabinets, core::u32 left,
                                  core::u32 top) {
      if (cabinets == nullptr) {
        return;
      }
      for (const JsonValue &cabinet : cabinets->items()) {
        if (!cabinet.is_object()) {
          continue;
        }
        const auto [x, y] = place_of(cabinet);
        const auto [w, h] = extent_of(cabinet, {"resolution", "size"});
        if (w == 0U || h == 0U) {
          continue;
        }
        mapped_width = std::max(mapped_width, far_edge(far_edge(left, x), w));
        mapped_height = std::max(mapped_height, far_edge(far_edge(top, y), h));
        ++screen.cabinets;
      }
    };
    if (const JsonValue *const canvases = entry.find("canvases")) {
      for (const JsonValue &canvas : canvases->items()) {
        if (!canvas.is_object()) {
          continue;
        }
        const auto [canvas_x, canvas_y] = place_of(canvas);
        auto [width, height] = extent_of(canvas, {"size", "resolution"});
        const auto [rect_width, rect_height] = size_at(canvas, "rectSize");
        width = std::max(width, rect_width);
        height = std::max(height, rect_height);
        declared_width = std::max(declared_width, far_edge(canvas_x, width));
        declared_height = std::max(declared_height, far_edge(canvas_y, height));
        add_cabinets(canvas.find("cabinets"), canvas_x, canvas_y);
      }
    }
    add_cabinets(entry.find("cabinets"), 0U, 0U);
    const auto [entry_declared_width, entry_declared_height] =
        extent_of(entry, {"resolution", "size"});
    const auto from_origin = [](core::u32 edge, core::u32 origin) noexcept {
      return edge > origin ? edge - origin : 0U;
    };
    if (screen.cabinets != 0U) {
      screen.width = from_origin(mapped_width, origin_x);
      screen.height = from_origin(mapped_height, origin_y);
    } else {
      // A screen-level declared size has no canvas offset in it.
      screen.width = std::max(from_origin(declared_width, origin_x),
                              entry_declared_width);
      screen.height = std::max(from_origin(declared_height, origin_y),
                               entry_declared_height);
    }

    // In All-In-One mode layers place inputs on the screen; pick the layer
    // set for the screen's working mode.
    // Some firmware lists the layers themselves there, or under "layers".
    const JsonValue *layers{};
    if (const JsonValue *const sets = entry.find("layersInWorkingMode")) {
      for (const JsonValue &set : sets->items()) {
        const JsonValue *const candidate = set.find("layers");
        if (candidate == nullptr) {
          if (set.find("source") != nullptr && layers == nullptr) {
            layers = sets;
          }
          continue;
        }
        if (candidate->items().empty()) {
          continue;
        }
        if (layers == nullptr || layers == sets ||
            static_cast<int>(set.number_at("workingMode", -1.0)) ==
                working_mode) {
          layers = candidate;
        }
      }
    }
    if (layers == nullptr) {
      if (const JsonValue *const plain = entry.find("layers");
          plain != nullptr && plain->is_array() && !plain->items().empty()) {
        layers = plain;
      }
    }
    if (layers != nullptr && working_mode != 0) {
      std::vector<std::pair<double, topology::HardwareProcessorLayer>> ordered;
      for (const JsonValue &layer : layers->items()) {
        const Input *const input = input_for_group(
            static_cast<core::i64>(layer.number_at("source", -1.0)));
        topology::HardwareProcessorLayer placed{};
        placed.input = number_of(input);
        const auto [layout_x, layout_y] = place_of(layer);
        const core::u32 x = from_origin(layout_x, origin_x);
        const core::u32 y = from_origin(layout_y, origin_y);
        auto [width, height] = extent_of(layer, {"scaler", "size"});
        placed.source = topology::HardwarePixelRect{
            0U, 0U, input != nullptr ? input->width : 0U,
            input != nullptr ? input->height : 0U};
        if (const JsonValue *const cut = layer.find("cut");
            cut != nullptr && cut->bool_at("enable")) {
          if (const JsonValue *const rect = cut->find("rect")) {
            placed.source = topology::HardwarePixelRect{
                to_u32(rect->number_at("x")), to_u32(rect->number_at("y")),
                to_u32(rect->number_at("width")),
                to_u32(rect->number_at("height"))};
          }
        }
        if (placed.source.width == 0U || placed.source.height == 0U) {
          placed.source.width = width;
          placed.source.height = height;
        }
        if (width == 0U || height == 0U) {
          width = placed.source.width;
          height = placed.source.height;
        }
        placed.screen = topology::HardwarePixelRect{x, y, width, height};
        if (placed.screen.width == 0U || placed.screen.height == 0U) {
          continue;
        }
        ordered.emplace_back(layer.number_at("zOrder", 0.0), placed);
      }
      std::stable_sort(ordered.begin(), ordered.end(),
                       [](const auto &left, const auto &right) {
                         return left.first < right.first;
                       });
      for (auto &[order, placed] : ordered) {
        static_cast<void>(order);
        screen.layers.push_back(placed);
      }
    }
    if (screen.layers.empty() && screen.width != 0U && screen.height != 0U) {
      // A send-only controller shows its selected input pixel for pixel
      // from the top left. Which input is selected is not reported, so the
      // first live one is taken.
      core::u8 input = mapped.empty() ? 0U : mapped.front().number;
      for (const topology::HardwareProcessorInput &shown : mapped) {
        if (shown.signal) {
          input = shown.number;
          break;
        }
      }
      topology::HardwareProcessorLayer whole{};
      whole.input = input;
      whole.source =
          topology::HardwarePixelRect{0U, 0U, screen.width, screen.height};
      whole.screen = whole.source;
      screen.layers.push_back(whole);
    }
    screens.push_back(std::move(screen));
  }
  return screens;
}

Identity parse_identity(const JsonValue *monitor, const JsonValue *hardware) {
  Identity identity{};
  for (const JsonValue *source : {monitor, hardware}) {
    if (source == nullptr || !identity.name.empty()) {
      continue;
    }
    for (const std::string_view key :
         {"name", "deviceName", "customName", "productName"}) {
      const std::string_view value = source->string_at(key);
      if (!value.empty()) {
        identity.name = std::string{value};
        break;
      }
    }
  }
  if (hardware != nullptr) {
    for (const std::string_view key : {"model", "modelName", "productName"}) {
      const std::string_view value = hardware->string_at(key);
      if (!value.empty()) {
        identity.model = std::string{value};
        break;
      }
    }
  }
  if (identity.model.empty() && !identity.name.empty()) {
    // Controllers name themselves "<model>_<serial>" until renamed.
    const std::size_t underscore = identity.name.rfind('_');
    if (underscore != std::string::npos && underscore != 0U) {
      identity.model = identity.name.substr(0U, underscore);
    }
  }
  return identity;
}

std::string edid_request_body(core::u32 width, core::u32 height,
                              core::u32 refresh_millihertz) {
  const auto mode = [&] {
    JsonValue resolution = JsonValue::object();
    resolution.set("width", JsonValue::number(width));
    resolution.set("height", JsonValue::number(height));
    JsonValue body = JsonValue::object();
    body.set("resolution", std::move(resolution));
    body.set("refreshRate",
             JsonValue::number(static_cast<double>(refresh_millihertz) / 1000.0));
    body.set("isCustom", JsonValue::boolean(!standard_mode(width, height)));
    return body;
  };
  JsonValue body = mode();
  body.set("para", mode());
  return to_json(body);
}

std::string edid_path(core::i64 input_id) {
  return "/api/v1/device/input/" + std::to_string(input_id) + "/edid";
}

Poll poll(const HttpEndpoint &endpoint, std::chrono::milliseconds timeout) {
  Poll result{};
  result.node.reachability = topology::HardwareRuntimeReachability::Offline;
  std::string error;

  const auto read = [&](std::string_view path) -> std::optional<JsonValue> {
    Answer answer = get(endpoint, path, timeout);
    result.reached = result.reached || answer.answered;
    if (!answer.json.has_value() && error.empty()) {
      error = answer.error;
    }
    return std::move(answer.json);
  };

  const std::optional<JsonValue> sources = read(input_sources_path);
  if (!result.reached) {
    // Nothing answered: the rest would only wait out the same timeout.
    result.node.detail = error.empty() ? "no answer" : error;
    return result;
  }
  // The cabinets are asked for by name; firmware that refuses the parameter,
  // or answers it with an error, is asked again without it.
  const std::string before = error;
  std::optional<JsonValue> screens = read(screen_with_cabinets_path);
  if (!screens.has_value() || unwrap(*screens).data == nullptr) {
    error = before;
    screens = read(screen_path);
  }
  const std::optional<JsonValue> monitor = read(monitor_path);

  std::string refused;
  if (sources.has_value()) {
    const Unwrapped data = unwrap(*sources);
    if (data.data != nullptr) {
      result.inputs = parse_inputs(*data.data);
    } else {
      refused = data.error;
    }
  }
  if (screens.has_value()) {
    const Unwrapped data = unwrap(*screens);
    if (data.data != nullptr) {
      result.node.screens = parse_screens(*data.data, result.inputs);
      if (std::none_of(result.node.screens.begin(), result.node.screens.end(),
                       [](const topology::HardwareProcessorScreen &screen) {
                         return screen.width != 0U && screen.height != 0U;
                       })) {
        // Said so, rather than shown as a processor with no screen at all.
        error = result.node.screens.empty()
                    ? "reports no screens"
                    : "reports its screens without a size";
        result.node.screens.clear();
      }
    } else if (refused.empty()) {
      refused = data.error;
    }
  }
  const JsonValue *monitor_data{};
  if (monitor.has_value()) {
    monitor_data = unwrap(*monitor).data;
  }
  const Identity identity = parse_identity(monitor_data, nullptr);
  result.node.name = identity.name;
  result.node.model = identity.model;
  result.node.inputs = map_inputs(result.inputs);
  if (!refused.empty() && result.node.inputs.empty() &&
      result.node.screens.empty()) {
    // It answers, but will not say anything: VMP holds the lock, or the
    // firmware refuses. That is not "online" for a show.
    result.node.reachability =
        topology::HardwareRuntimeReachability::ConfigurationError;
    result.node.detail = refused;
    return result;
  }
  result.node.reachability = topology::HardwareRuntimeReachability::Online;
  result.node.detail = refused.empty() ? error : refused;
  return result;
}

std::optional<topology::HardwareDiscoveredProcessor>
identify(const HttpEndpoint &endpoint, std::chrono::milliseconds timeout) {
  const Answer reply = get(endpoint, screen_path, timeout);
  if (!reply.json.has_value()) {
    return std::nullopt;
  }
  const JsonValue *const answer = &*reply.json;
  // A COEX controller answers {"code":..., "data": {"screens": [...]}} or,
  // locked by VMP, a code and a message.
  const bool wrapped = answer->is_object() && answer->find("code") != nullptr &&
                       answer->find("message") != nullptr;
  const Unwrapped data = unwrap(*answer);
  const bool screens = data.data != nullptr && data.data->is_object() &&
                       data.data->find("screens") != nullptr;
  if (!wrapped && !screens) {
    return std::nullopt;
  }
  topology::HardwareDiscoveredProcessor found{};
  found.protocol = topology::HardwareProcessorProtocol::NovastarCoex;
  found.address = endpoint.port == default_port
                      ? endpoint.host
                      : endpoint.host + ':' + std::to_string(endpoint.port);
  if (const Answer monitor = get(endpoint, monitor_path, timeout);
      monitor.json.has_value()) {
    const Identity identity =
        parse_identity(unwrap(*monitor.json).data, nullptr);
    found.name = identity.name;
    found.model = identity.model;
  }
  if (found.name.empty()) {
    found.name = "NovaStar processor";
  }
  return found;
}

WriteResult write_edid(const HttpEndpoint &endpoint, core::i64 input_id,
                       core::u32 width, core::u32 height,
                       core::u32 refresh_millihertz,
                       std::chrono::milliseconds timeout) {
  HttpRequest request{};
  request.method = "PUT";
  request.endpoint = endpoint;
  request.path = edid_path(input_id);
  request.body = edid_request_body(width, height, refresh_millihertz);
  request.timeout = timeout;
  const HttpResult result = http_request(request);
  if (!result.response.has_value()) {
    return WriteResult{false, result.error};
  }
  if (result.response->status != 200) {
    return WriteResult{false,
                       "HTTP " + std::to_string(result.response->status)};
  }
  const std::optional<JsonValue> answer = parse_json(result.response->body);
  if (!answer.has_value()) {
    return WriteResult{false, "answer is not JSON"};
  }
  const Unwrapped data = unwrap(*answer);
  if (!data.error.empty()) {
    return WriteResult{false, data.error};
  }
  return WriteResult{true, {}};
}

} // namespace mediaserver::devices::coex
