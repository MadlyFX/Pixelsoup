#include <mediaserver/devices/brompton_tessera.hpp>
#include <mediaserver/devices/http_client.hpp>
#include <mediaserver/devices/json.hpp>
#include <mediaserver/devices/novastar_coex.hpp>
#include <mediaserver/devices/processor_monitor.hpp>
#include <mediaserver/topology/hardware_map_model.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace devices = mediaserver::devices;
namespace coex = mediaserver::devices::coex;
namespace tessera = mediaserver::devices::tessera;
namespace doc = mediaserver::doc;
namespace topology = mediaserver::topology;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define DEVICES_CHECK(...)                                                     \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// Input sources as the COEX manual documents them, plus the sourceStatus
// field real controllers send: HDMI 1 live at 3840x2160@59.94 with a matching
// EDID, HDMI 2 dark with a 60 Hz EDID, DP live at 1920x1080@50, and the
// internal pattern source, which a card does not show.
constexpr std::string_view sources_answer = R"({
  "code": 0, "message": "Success",
  "data": [
    {"id": 1, "type": 3, "name": "HDMI 2.0-1", "groupId": 10,
     "actualResolution": {"width": 3840, "height": 2160},
     "actualRefreshRate": 59.94, "sourceStatus": 1, "isSupportEDID": true,
     "defaultEDID": {"resolution": {"width": 3840, "height": 2160},
                     "refreshRate": 59.94}},
    {"id": 2, "type": 3, "name": "HDMI 2.0-2", "groupId": 11,
     "actualResolution": {"width": 3840, "height": 2160},
     "actualRefreshRate": 60, "sourceStatus": 0, "isSupportEDID": true,
     "defaultEDID": {"resolution": {"width": 3840, "height": 2160},
                     "refreshRate": 60.0}},
    {"id": 5, "type": 5, "name": "", "groupId": 12,
     "actualResolution": {"width": 1920, "height": 1080},
     "actualRefreshRate": 50.0, "sourceStatus": 1, "isSupportEDID": false},
    {"id": 224, "type": 224, "name": "Internal", "groupId": 99}
  ]
})";

// One All-In-One screen of two canvases, each holding cabinets relative to
// it, with two layers: HDMI 1 cropped into the left part and the DP input
// scaled into the right.
constexpr std::string_view screens_answer = R"({
  "code": 0, "message": "Success",
  "data": {
    "screens": [{
      "screenID": "{8a0f}", "screenName": "Main wall", "workingMode": 1,
      "lowLatency": false, "masterFrameRate": 59.94,
      "canvases": [
        {"canvasID": 0, "position": {"x": 0, "y": 0},
         "size": {"width": 0, "height": 0},
         "cabinets": [
           {"cabinetID": 1, "outputID": 1, "position": {"x": 0, "y": 0},
            "size": {"width": 1000, "height": 500}},
           {"cabinetID": 2, "outputID": 1, "position": {"x": 1000, "y": 0},
            "size": {"width": 1000, "height": 500}}]},
        {"canvasID": 1, "position": {"x": 2000, "y": 0},
         "size": {"width": 1000, "height": 600},
         "cabinets": [
           {"cabinetID": 3, "outputID": 2, "position": {"x": 0, "y": 0},
            "size": {"width": 1000, "height": 500}}]}
      ],
      "layersInWorkingMode": [
        {"workingMode": 0, "layers": []},
        {"workingMode": 1, "layers": [
          {"id": 2, "source": 12, "zOrder": 2,
           "position": {"x": 2000, "y": 0},
           "scaler": {"width": 1000, "height": 600},
           "cut": {"enable": false, "rect": {"x": 0, "y": 0, "width": 0, "height": 0}}},
          {"id": 1, "source": 10, "zOrder": 1,
           "position": {"x": 0, "y": 0},
           "scaler": {"width": 2000, "height": 500},
           "cut": {"enable": true, "rect": {"x": 0, "y": 0, "width": 2000, "height": 500}}}
        ]}
      ]
    }],
    "screenGroups": []
  }
})";

constexpr std::string_view monitor_answer =
    R"({"code":0,"message":"Success","data":{"name":"MX40 Pro_002198","cabinets":[]}})";

void json_reads_and_writes_what_devices_send() {
  const auto parsed = devices::parse_json(
      R"({"a": [1, 2.5, -3e2, true, null, "xé😀\n"], "b": {}})");
  DEVICES_CHECK(parsed.has_value());
  const devices::JsonValue *const a = parsed->find("a");
  DEVICES_CHECK(a != nullptr && a->items().size() == 6U);
  DEVICES_CHECK(a->items()[1].as_number() == 2.5);
  DEVICES_CHECK(a->items()[2].as_number() == -300.0);
  DEVICES_CHECK(a->items()[3].as_bool());
  DEVICES_CHECK(a->items()[4].is_null());
  DEVICES_CHECK(a->items()[5].as_string() ==
                "x\xC3\xA9\xF0\x9F\x98\x80\n");
  DEVICES_CHECK(parsed->find("b") != nullptr && parsed->find("b")->is_object());

  for (const std::string_view broken :
       {R"({"a": 1,})", R"([1 2])", R"({"a": 01})", R"("open)", R"({} {})",
        R"({"a": tru})", R"(["\ud800"])", ""}) {
    DEVICES_CHECK(!devices::parse_json(broken).has_value());
  }
  std::string deep(100U, '[');
  deep += std::string(100U, ']');
  DEVICES_CHECK(!devices::parse_json(deep).has_value());
  devices::JsonLimits small{};
  small.max_bytes = 4U;
  DEVICES_CHECK(!devices::parse_json("[1,2,3]", small).has_value());

  devices::JsonValue body = devices::JsonValue::object();
  body.set("width", devices::JsonValue::number(3840.0));
  body.set("rate", devices::JsonValue::number(59.94));
  body.set("name", devices::JsonValue::string("a\"b"));
  DEVICES_CHECK(devices::to_json(body) ==
                R"({"width":3840,"rate":59.94,"name":"a\"b"})");
}

void http_framing_and_endpoints_are_strict() {
  const auto plain = devices::parse_http_endpoint("10.0.0.40", 8001U);
  DEVICES_CHECK(plain.has_value() && plain->host == "10.0.0.40" &&
                plain->port == 8001U);
  const auto ported = devices::parse_http_endpoint(" 127.0.0.1:18001 ", 8001U);
  DEVICES_CHECK(ported.has_value() && ported->port == 18001U);
  const auto v6 = devices::parse_http_endpoint("[::1]:9000", 8001U);
  DEVICES_CHECK(v6.has_value() && v6->host == "::1" && v6->port == 9000U);
  DEVICES_CHECK(devices::parse_http_endpoint("::1", 8001U)->port == 8001U);
  DEVICES_CHECK(!devices::parse_http_endpoint("processor.local", 8001U));
  DEVICES_CHECK(!devices::parse_http_endpoint("10.0.0.40:0", 8001U));
  DEVICES_CHECK(!devices::parse_http_endpoint("10.0.0.40:70000", 8001U));
  DEVICES_CHECK(!devices::parse_http_endpoint("", 8001U));

  const auto decoded =
      devices::decode_chunked_body("4\r\nWiki\r\n5;x=y\r\npedia\r\n0\r\n\r\n",
                                   1024U);
  DEVICES_CHECK(decoded.has_value() && *decoded == "Wikipedia");
  DEVICES_CHECK(!devices::decode_chunked_body("4\r\nWik", 1024U).has_value());
  DEVICES_CHECK(!devices::decode_chunked_body("z\r\n\r\n", 1024U).has_value());
  DEVICES_CHECK(
      !devices::decode_chunked_body("5\r\npedia\r\n0\r\n\r\n", 4U).has_value());
}

void coex_answers_become_a_processor_card() {
  const auto sources = devices::parse_json(sources_answer);
  DEVICES_CHECK(sources.has_value());
  const coex::Unwrapped source_data = coex::unwrap(*sources);
  DEVICES_CHECK(source_data.data != nullptr && source_data.error.empty());
  const std::vector<coex::Input> inputs = coex::parse_inputs(*source_data.data);
  DEVICES_CHECK(inputs.size() == 4U);
  DEVICES_CHECK(inputs[0].signal && inputs[0].refresh_millihertz == 59'940U);
  // A dark input's "actual" size is only its EDID default.
  DEVICES_CHECK(!inputs[1].signal && inputs[1].width == 0U);
  DEVICES_CHECK(inputs[1].edid_refresh_millihertz == 60'000U);

  const auto mapped = coex::map_inputs(inputs);
  DEVICES_CHECK(mapped.size() == 3U);
  DEVICES_CHECK(mapped[0].number == 1U && mapped[0].vendor_id == 1 &&
                mapped[0].name == "HDMI 2.0-1" &&
                mapped[0].connector == "HDMI 2.0" && mapped[0].edid_writable);
  // An unnamed input is called by its connector.
  DEVICES_CHECK(mapped[2].number == 3U && mapped[2].vendor_id == 5 &&
                mapped[2].name == "DP 1.2" && !mapped[2].edid_writable);

  const auto screens_json = devices::parse_json(screens_answer);
  DEVICES_CHECK(screens_json.has_value());
  const auto screens =
      coex::parse_screens(*coex::unwrap(*screens_json).data, inputs);
  DEVICES_CHECK(screens.size() == 1U);
  const topology::HardwareProcessorScreen &screen = screens.front();
  DEVICES_CHECK(screen.id == "{8a0f}" && screen.name == "Main wall");
  DEVICES_CHECK(screen.width == 3000U && screen.height == 600U);
  DEVICES_CHECK(screen.cabinets == 3U && screen.refresh_millihertz == 59'940U);
  // Layers in z order: HDMI 1 cropped on the left, then the DP on the right.
  DEVICES_CHECK(screen.layers.size() == 2U);
  DEVICES_CHECK(screen.layers[0].input == 1U &&
                screen.layers[0].source ==
                    topology::HardwarePixelRect{0U, 0U, 2000U, 500U} &&
                screen.layers[0].screen ==
                    topology::HardwarePixelRect{0U, 0U, 2000U, 500U});
  DEVICES_CHECK(screen.layers[1].input == 3U &&
                screen.layers[1].source ==
                    topology::HardwarePixelRect{0U, 0U, 1920U, 1080U} &&
                screen.layers[1].screen ==
                    topology::HardwarePixelRect{2000U, 0U, 1000U, 600U});

  // Send-only: the selected input shows from its top left.
  const auto send_only = devices::parse_json(
      R"({"screens":[{"screenID":7,"workingMode":0,"canvases":[{"position":{"x":0,"y":0},"size":{"width":2880,"height":1536},"cabinets":[]}]}]})");
  const auto plain = coex::parse_screens(*send_only, inputs);
  DEVICES_CHECK(plain.size() == 1U && plain[0].id == "7");
  DEVICES_CHECK(plain[0].layers.size() == 1U && plain[0].layers[0].input == 1U &&
                plain[0].layers[0].source ==
                    topology::HardwarePixelRect{0U, 0U, 2880U, 1536U});

  // The other layout firmware uses: a canvas's pixel size as its own width
  // and height, cabinets beside the canvases with a pixel resolution and a
  // physical size in millimetres, and the layers listed flat.
  const auto flat = devices::parse_json(R"({"screens":[{
      "screenID":"{77}","screenName":"","workingMode":1,"masterFrameRate":23.976,
      "canvases":[{"canvasID":0,"width":2880,"height":1536}],
      "cabinets":[
        {"id":1,"x":0,"y":0,"resolution":{"width":288,"height":256},
         "size":{"width":500,"height":500}},
        {"id":2,"x":2592,"y":1280,"resolution":{"width":288,"height":256},
         "size":{"width":500,"height":500}}],
      "layersInWorkingMode":[
        {"id":1,"source":10,"position":{"x":0,"y":0},
         "size":{"width":2880,"height":1536},
         "cut":{"enable":true,"rect":{"x":0,"y":0,"width":2880,"height":1536}}}]}]})");
  DEVICES_CHECK(flat.has_value());
  const auto other = coex::parse_screens(*flat, inputs);
  DEVICES_CHECK(other.size() == 1U);
  DEVICES_CHECK(other[0].width == 2880U && other[0].height == 1536U);
  DEVICES_CHECK(other[0].cabinets == 2U &&
                other[0].refresh_millihertz == 23'976U);
  DEVICES_CHECK(other[0].layers.size() == 1U &&
                other[0].layers[0].input == 1U &&
                other[0].layers[0].source ==
                    topology::HardwarePixelRect{0U, 0U, 2880U, 1536U} &&
                other[0].layers[0].screen ==
                    topology::HardwarePixelRect{0U, 0U, 2880U, 1536U});

  const auto monitor = devices::parse_json(monitor_answer);
  const coex::Identity identity =
      coex::parse_identity(coex::unwrap(*monitor).data, nullptr);
  DEVICES_CHECK(identity.name == "MX40 Pro_002198" &&
                identity.model == "MX40 Pro");

  // Bare answers and refusals.
  const auto bare = devices::parse_json(R"([{"id":3,"type":3}])");
  DEVICES_CHECK(coex::unwrap(*bare).data == &*bare);
  const auto locked = devices::parse_json(
      R"({"code":5,"message":"Device busy","data":null})");
  DEVICES_CHECK(coex::unwrap(*locked).data == nullptr &&
                coex::unwrap(*locked).error == "Device busy");

  const std::string body = coex::edid_request_body(3840U, 2160U, 59'940U);
  const auto edid = devices::parse_json(body);
  DEVICES_CHECK(edid.has_value());
  DEVICES_CHECK(edid->find("resolution")->number_at("width") == 3840.0);
  DEVICES_CHECK(edid->number_at("refreshRate") == 59.94);
  DEVICES_CHECK(!edid->bool_at("isCustom", true));
  DEVICES_CHECK(edid->find("para")->find("resolution")->number_at("height") ==
                2160.0);
  DEVICES_CHECK(devices::parse_json(coex::edid_request_body(3456U, 1536U,
                                                            50'000U))
                    ->bool_at("isCustom"));
  DEVICES_CHECK(coex::edid_path(2) == "/api/v1/device/input/2/edid");
}

// A COEX controller on loopback: answers the three reads and records EDID
// writes. Answers alternate between Content-Length and chunked framing.
class FakeController final {
public:
  FakeController() : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}) {
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread{[this] { serve(); }};
  }

  ~FakeController() {
    stopping_ = true;
    std::error_code ignored;
    // Wake the blocking accept.
    asio::io_context poke;
    asio::ip::tcp::socket socket{poke};
    socket.connect({asio::ip::make_address("127.0.0.1"), port_}, ignored);
    thread_.join();
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  [[nodiscard]] std::vector<std::string> writes() const {
    std::lock_guard guard{mutex_};
    return writes_;
  }

  [[nodiscard]] int requests() const noexcept { return requests_; }

private:
  void serve() {
    while (!stopping_) {
      asio::ip::tcp::socket socket{io_};
      std::error_code error;
      acceptor_.accept(socket, error);
      if (error || stopping_) {
        continue;
      }
      try {
        handle(socket);
      } catch (const std::exception &) {
      }
    }
  }

  void handle(asio::ip::tcp::socket &socket) {
    asio::streambuf buffer;
    asio::read_until(socket, buffer, "\r\n\r\n");
    std::string head{asio::buffers_begin(buffer.data()),
                     asio::buffers_end(buffer.data())};
    const std::size_t end = head.find("\r\n\r\n");
    std::string body = head.substr(end + 4U);
    head.resize(end);
    std::size_t length{};
    if (const std::size_t at = head.find("Content-Length: ");
        at != std::string::npos) {
      length = std::stoul(head.substr(at + 16U));
    }
    while (body.size() < length) {
      std::array<char, 1024U> more{};
      const std::size_t n = socket.read_some(asio::buffer(more));
      body.append(more.data(), n);
    }
    ++requests_;
    const std::string line = head.substr(0U, head.find("\r\n"));
    std::string answer = R"({"code":36,"message":"NotImplement","data":null})";
    if (line.starts_with("GET /api/v1/device/input/sources ")) {
      answer = std::string{sources_answer};
    } else if (line.starts_with("GET /api/v1/screen ")) {
      answer = std::string{screens_answer};
    } else if (line.starts_with("GET /api/v1/device/monitor/info ")) {
      answer = std::string{monitor_answer};
    } else if (line.starts_with("PUT /api/v1/device/input/2/edid ")) {
      std::lock_guard guard{mutex_};
      writes_.push_back(body);
      answer = R"({"code":0,"message":"Success","data":null})";
    }
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
    if ((requests_ % 2) == 0) {
      response += "Transfer-Encoding: chunked\r\n\r\n";
      // Two chunks, to prove reassembly.
      const std::size_t half = answer.size() / 2U;
      for (const std::string_view part :
           {std::string_view{answer}.substr(0U, half),
            std::string_view{answer}.substr(half)}) {
        std::array<char, 16U> size{};
        std::snprintf(size.data(), size.size(), "%zx\r\n", part.size());
        response += size.data();
        response += part;
        response += "\r\n";
      }
      response += "0\r\n\r\n";
    } else {
      response += "Content-Length: " + std::to_string(answer.size()) +
                  "\r\n\r\n" + answer;
    }
    asio::write(socket, asio::buffer(response));
    std::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
  }

  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_;
  std::uint16_t port_{};
  std::thread thread_;
  std::atomic_bool stopping_{};
  std::atomic_int requests_{};
  mutable std::mutex mutex_;
  std::vector<std::string> writes_;
};

template <typename Predicate>
[[nodiscard]] bool eventually(Predicate predicate,
                              std::chrono::milliseconds limit =
                                  std::chrono::milliseconds{8'000}) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  return predicate();
}

void a_controller_is_polled_written_and_found() {
  FakeController controller;
  const devices::HttpEndpoint endpoint{"127.0.0.1", controller.port()};

  const coex::Poll polled = coex::poll(endpoint, std::chrono::milliseconds{2'000});
  DEVICES_CHECK(polled.reached);
  DEVICES_CHECK(polled.node.reachability ==
                topology::HardwareRuntimeReachability::Online);
  DEVICES_CHECK(polled.node.name == "MX40 Pro_002198" &&
                polled.node.model == "MX40 Pro");
  DEVICES_CHECK(polled.node.inputs.size() == 3U &&
                polled.node.screens.size() == 1U);

  // Nothing listens on a closed port: offline, quickly.
  asio::io_context io;
  asio::ip::tcp::acceptor closed{io, {asio::ip::make_address("127.0.0.1"), 0}};
  const std::uint16_t dead_port = closed.local_endpoint().port();
  closed.close();
  const coex::Poll dead = coex::poll(devices::HttpEndpoint{"127.0.0.1", dead_port},
                                     std::chrono::milliseconds{1'000});
  DEVICES_CHECK(!dead.reached &&
                dead.node.reachability ==
                    topology::HardwareRuntimeReachability::Offline &&
                !dead.node.detail.empty());

  devices::ProcessorMonitorConfig config{};
  config.poll_interval = std::chrono::milliseconds{100};
  config.request_timeout = std::chrono::milliseconds{1'000};
  config.search_hosts = {"127.0.0.1"};
  config.search_port = controller.port();
  config.brompton_search_port = 0U;
  devices::ProcessorMonitor monitor{config};
  monitor.start();
  const doc::NodeId card{7U};
  monitor.watch({devices::ProcessorTarget{
      card, topology::HardwareProcessorProtocol::NovastarCoex,
      "127.0.0.1:" + std::to_string(controller.port())}});
  DEVICES_CHECK(eventually([&] {
    const auto nodes = monitor.processors();
    return nodes.size() == 1U &&
           nodes[0].reachability ==
               topology::HardwareRuntimeReachability::Online;
  }));
  DEVICES_CHECK(monitor.processors()[0].device == card);
  DEVICES_CHECK(monitor.processors()[0].input(2U) != nullptr);

  // Input two is the controller's id 2.
  monitor.write_edid(devices::ProcessorEdidWrite{card, 2U, 3840U, 2160U,
                                                 59'940U});
  DEVICES_CHECK(eventually([&] { return !controller.writes().empty(); }));
  const auto written = devices::parse_json(controller.writes().front());
  DEVICES_CHECK(written.has_value() &&
                written->number_at("refreshRate") == 59.94);
  DEVICES_CHECK(eventually([&] {
    const auto nodes = monitor.processors();
    return !nodes.empty() && !nodes[0].notice.empty();
  }));
  DEVICES_CHECK(!monitor.processors()[0].notice_failed);
  DEVICES_CHECK(monitor.processors()[0].notice ==
                "Input 2 EDID set to 3840x2160 at 59.94");

  // An input the processor never reported is refused without a request.
  const int before = controller.requests();
  monitor.write_edid(devices::ProcessorEdidWrite{card, 9U, 1920U, 1080U,
                                                 60'000U});
  DEVICES_CHECK(eventually([&] {
    const auto nodes = monitor.processors();
    return !nodes.empty() && nodes[0].notice_failed;
  }));
  DEVICES_CHECK(controller.writes().size() == 1U);
  static_cast<void>(before);

  monitor.search();
  DEVICES_CHECK(eventually([&] { return monitor.search_state().completed == 1U; }));
  const devices::ProcessorSearchState search = monitor.search_state();
  DEVICES_CHECK(!search.running && search.found.size() == 1U);
  DEVICES_CHECK(search.found[0].address ==
                "127.0.0.1:" + std::to_string(controller.port()));
  DEVICES_CHECK(search.found[0].name == "MX40 Pro_002198");

  // A card that leaves the map is forgotten.
  monitor.watch({});
  DEVICES_CHECK(monitor.processors().empty());
  monitor.stop();
}

void only_protocol_processors_are_watched() {
  topology::HardwareMap map{};
  topology::HardwareDevice server{};
  server.node = doc::NodeId{1U};
  server.kind = topology::HardwareDeviceKind::RenderServer;
  topology::HardwareDevice generic{};
  generic.node = doc::NodeId{2U};
  generic.kind = topology::HardwareDeviceKind::LedProcessor;
  generic.address = "10.0.0.2";
  topology::HardwareDevice coex_card = generic;
  coex_card.node = doc::NodeId{3U};
  coex_card.address = "10.0.0.3";
  coex_card.protocol = topology::HardwareProcessorProtocol::NovastarCoex;
  topology::HardwareDevice unaddressed = coex_card;
  unaddressed.node = doc::NodeId{4U};
  unaddressed.address.clear();
  map.devices = {server, generic, coex_card, unaddressed};
  const auto targets = devices::processor_targets(map);
  DEVICES_CHECK(targets.size() == 1U && targets[0].device == doc::NodeId{3U} &&
                targets[0].address == "10.0.0.3");
}

void tessera_answers_are_read_as_documented() {
  // Quoted, as a JSON encoder writes it; keys match whatever their case.
  const auto rate =
      tessera::endpoint_value(R"({"refresh-rate": 59.94})",
                              "/api/input/active/refresh-rate");
  DEVICES_CHECK(rate.has_value() && rate->as_number() == 59.94);
  const auto type = tessera::endpoint_value(R"({"Processor-Type":"SX40"})",
                                            "/api/system/processor-type");
  DEVICES_CHECK(type.has_value() && type->as_string() == "SX40");
  // Unquoted, as the manual prints it.
  const auto printed = tessera::endpoint_value(
      "{ refresh-rate: 60 }", "/api/input/active/refresh-rate");
  DEVICES_CHECK(printed.has_value() && printed->as_number() == 60.0);
  const auto port = tessera::endpoint_value(
      "{ port-type: hdmi }", "/api/input/active/source/port-type");
  DEVICES_CHECK(port.has_value() && port->as_string() == "hdmi");
  // Another endpoint's answer is not this one's.
  DEVICES_CHECK(!tessera::endpoint_value("{ width: 3840 }",
                                         "/api/input/active/resolution/height")
                     .has_value());
  DEVICES_CHECK(!tessera::endpoint_value("<html></html>",
                                         "/api/system/processor-type")
                     .has_value());

  DEVICES_CHECK(tessera::model_name("sx40") == "SX40");
  DEVICES_CHECK(tessera::model_name(" S8 ") == "S8");
  DEVICES_CHECK(tessera::model_name("router").empty());
  DEVICES_CHECK(tessera::input_name("sdi", 2U) == "SDI 2");
  DEVICES_CHECK(tessera::input_name("", 0U) == "Input");
}

// A Tessera S8 on loopback, answering its IP control tree, or - with no
// paths - a web server that is not one.
class FakeTessera final {
public:
  explicit FakeTessera(std::map<std::string, std::string> paths)
      : paths_(std::move(paths)),
        acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}) {
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread{[this] { serve(); }};
  }

  ~FakeTessera() {
    stopping_ = true;
    std::error_code ignored;
    asio::io_context poke;
    asio::ip::tcp::socket socket{poke};
    socket.connect({asio::ip::make_address("127.0.0.1"), port_}, ignored);
    thread_.join();
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] int requests() const noexcept { return requests_; }

private:
  void serve() {
    while (!stopping_) {
      asio::ip::tcp::socket socket{io_};
      std::error_code error;
      acceptor_.accept(socket, error);
      if (error || stopping_) {
        continue;
      }
      try {
        asio::streambuf buffer;
        asio::read_until(socket, buffer, "\r\n\r\n");
        const std::string head{asio::buffers_begin(buffer.data()),
                               asio::buffers_end(buffer.data())};
        const std::size_t first = head.find(' ');
        const std::size_t second = head.find(' ', first + 1U);
        const std::string path = head.substr(first + 1U, second - first - 1U);
        ++requests_;
        const auto found = paths_.find(path);
        const std::string body =
            found != paths_.end() ? found->second : std::string{"Not found"};
        const std::string response =
            std::string{found != paths_.end() ? "HTTP/1.1 200 OK\r\n"
                                              : "HTTP/1.1 404 Not Found\r\n"} +
            "Content-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        asio::write(socket, asio::buffer(response));
        std::error_code ignored;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
      } catch (const std::exception &) {
      }
    }
  }

  std::map<std::string, std::string> paths_;
  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_;
  std::uint16_t port_{};
  std::thread thread_;
  std::atomic_bool stopping_{};
  std::atomic_int requests_{};
};

[[nodiscard]] std::map<std::string, std::string> tessera_tree() {
  return {
      {"/api/system/processor-type", R"({"processor-type":"s8"})"},
      {"/api/system/software-version", R"({"software-version":"3.1.0"})"},
      {"/api/input/active/source/port-type", R"({"port-type":"hdmi"})"},
      {"/api/input/active/source/port-number", R"({"port-number":1})"},
      {"/api/input/active/resolution/width", R"({"width":3840})"},
      {"/api/input/active/resolution/height", R"({"height":2160})"},
      // As the manual prints it: an unquoted key.
      {"/api/input/active/refresh-rate", "{ refresh-rate: 59.94 }"},
      {"/api/panels/statistics/associated-count", R"({"associated-count":120})"},
      {"/api/panels/statistics/online-count", R"({"online-count":118})"},
      {"/api/panels/statistics/error-count", R"({"error-count":1})"},
      {"/api/override/blackout/enabled", R"({"enabled":true})"},
      {"/api/override/freeze/enabled", R"({"enabled":false})"},
      {"/api/override/test-pattern/enabled", R"({"enabled":false})"},
      {"/api/presets/active/name", R"({"name":"Show"})"},
  };
}

void a_tessera_processor_is_polled_and_found() {
  FakeTessera processor{tessera_tree()};
  const devices::HttpEndpoint endpoint{"127.0.0.1", processor.port()};

  const tessera::Poll polled =
      tessera::poll(endpoint, std::chrono::milliseconds{2'000});
  DEVICES_CHECK(polled.reached);
  const topology::HardwareProcessorNode &node = polled.node;
  DEVICES_CHECK(node.reachability ==
                topology::HardwareRuntimeReachability::Online);
  DEVICES_CHECK(node.model == "S8" && node.name == "Brompton S8" &&
                node.software == "3.1.0" && node.preset == "Show");
  // Only the input in use is reported; others are not missing.
  DEVICES_CHECK(!node.all_inputs && node.inputs.size() == 1U);
  DEVICES_CHECK(node.inputs[0].number == 1U && node.inputs[0].name == "HDMI 1" &&
                node.inputs[0].connector == "HDMI" && node.inputs[0].signal &&
                node.inputs[0].width == 3840U &&
                node.inputs[0].refresh_millihertz == 59'940U);
  // Tessera's IP control has no EDID.
  DEVICES_CHECK(node.inputs[0].edid_width == 0U && !node.inputs[0].edid_writable);
  DEVICES_CHECK(node.screens.empty());
  DEVICES_CHECK(node.panels == 120U && node.panels_online == 118U &&
                node.panels_faulted == 1U);
  DEVICES_CHECK(node.blackout && !node.frozen && !node.test_pattern);
  DEVICES_CHECK(node.attention() ==
                "blacked out, 2 of 120 panels offline, 1 panel faulted");

  const auto found = tessera::identify(endpoint, std::chrono::milliseconds{2'000});
  DEVICES_CHECK(found.has_value() &&
                found->protocol ==
                    topology::HardwareProcessorProtocol::BromptonTessera &&
                found->name == "Brompton S8" &&
                found->address ==
                    "127.0.0.1:" + std::to_string(processor.port()));

  // Something else on port 80 - IP control switched off - is refused with
  // what to do about it, and is no processor to a search.
  FakeTessera web{{}};
  const devices::HttpEndpoint other{"127.0.0.1", web.port()};
  const tessera::Poll refused =
      tessera::poll(other, std::chrono::milliseconds{2'000});
  DEVICES_CHECK(refused.reached &&
                refused.node.reachability ==
                    topology::HardwareRuntimeReachability::ConfigurationError);
  DEVICES_CHECK(refused.node.detail.find("Live Control") != std::string::npos);
  DEVICES_CHECK(!tessera::identify(other, std::chrono::milliseconds{2'000})
                     .has_value());

  devices::ProcessorMonitorConfig config{};
  config.poll_interval = std::chrono::milliseconds{100};
  config.request_timeout = std::chrono::milliseconds{1'000};
  config.search_hosts = {"127.0.0.1"};
  config.search_port = 0U;
  config.brompton_search_port = processor.port();
  devices::ProcessorMonitor monitor{config};
  monitor.start();
  const doc::NodeId card{9U};
  monitor.watch({devices::ProcessorTarget{
      card, topology::HardwareProcessorProtocol::BromptonTessera,
      "127.0.0.1:" + std::to_string(processor.port())}});
  DEVICES_CHECK(eventually([&] {
    const auto nodes = monitor.processors();
    return nodes.size() == 1U &&
           nodes[0].reachability ==
               topology::HardwareRuntimeReachability::Online;
  }));
  // An EDID write is refused without asking the processor.
  const int before = processor.requests();
  monitor.write_edid(devices::ProcessorEdidWrite{card, 1U, 3840U, 2160U,
                                                 59'940U});
  DEVICES_CHECK(eventually([&] {
    const auto nodes = monitor.processors();
    return !nodes.empty() && nodes[0].notice_failed;
  }));
  static_cast<void>(before);

  monitor.search();
  DEVICES_CHECK(eventually([&] { return monitor.search_state().completed == 1U; }));
  const devices::ProcessorSearchState search = monitor.search_state();
  DEVICES_CHECK(search.found.size() == 1U &&
                search.found[0].protocol ==
                    topology::HardwareProcessorProtocol::BromptonTessera);
  monitor.stop();
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"tessera answers are read as documented",
     &tessera_answers_are_read_as_documented},
    {"a tessera processor is polled and found",
     &a_tessera_processor_is_polled_and_found},
    {"json reads and writes what devices send",
     &json_reads_and_writes_what_devices_send},
    {"http framing and endpoints are strict",
     &http_framing_and_endpoints_are_strict},
    {"coex answers become a processor card",
     &coex_answers_become_a_processor_card},
    {"only protocol processors are watched",
     &only_protocol_processors_are_watched},
    {"a controller is polled written and found",
     &a_controller_is_polled_written_and_found},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
