#include <mediaserver/devices/processor_monitor.hpp>

#include <mediaserver/devices/http_client.hpp>
#include <mediaserver/devices/novastar_coex.hpp>
#include <mediaserver/net/cluster_discovery.hpp>
#include <mediaserver/topology/hardware_signal.hpp>

#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>

namespace mediaserver::devices {

namespace {

using Clock = std::chrono::steady_clock;

struct Watched final {
  ProcessorTarget target;
  topology::HardwareProcessorNode node;
  // The controller's input list, to address an EDID write.
  std::vector<coex::Input> inputs;
  Clock::time_point due{};
  bool ever_reached{};
};

// Hosts to try in one search: loopback, then each local subnet's hosts.
[[nodiscard]] std::vector<asio::ip::address_v4>
search_hosts(const ProcessorMonitorConfig &config) {
  std::vector<asio::ip::address_v4> hosts;
  if (!config.search_hosts.empty()) {
    for (const std::string &text : config.search_hosts) {
      std::error_code error;
      const auto address = asio::ip::make_address_v4(text, error);
      if (!error) {
        hosts.push_back(address);
      }
    }
    return hosts;
  }
  hosts.push_back(asio::ip::address_v4::loopback());
  std::vector<net::LocalIpv4Interface> interfaces;
  try {
    interfaces = net::local_ipv4_interfaces();
  } catch (const std::bad_alloc &) {
    return hosts;
  }
  for (const net::LocalIpv4Interface &local : interfaces) {
    std::uint32_t mask = local.netmask.to_uint();
    // A subnet larger than a /22 is walked only around this machine.
    if (mask < 0xFFFFFC00U) {
      mask = 0xFFFFFC00U;
    }
    const std::uint32_t own = local.address.to_uint();
    const std::uint32_t network = own & mask;
    const std::uint32_t broadcast = network | ~mask;
    for (std::uint32_t host = network + 1U; host < broadcast; ++host) {
      if (host != own) {
        hosts.emplace_back(host);
      }
    }
  }
  std::sort(hosts.begin(), hosts.end());
  hosts.erase(std::unique(hosts.begin(), hosts.end()), hosts.end());
  return hosts;
}

// The hosts in `hosts` that accept a TCP connection on `port` within the
// timeout, tried `parallel` at a time.
[[nodiscard]] std::vector<asio::ip::address_v4>
open_hosts(const std::vector<asio::ip::address_v4> &hosts, std::uint16_t port,
           std::chrono::milliseconds timeout, std::size_t parallel,
           const std::atomic_bool &stop) {
  std::vector<asio::ip::address_v4> open;
  asio::io_context io;
  struct Probe final {
    explicit Probe(asio::io_context &context) : socket(context), timer(context) {}
    asio::ip::tcp::socket socket;
    asio::steady_timer timer;
    asio::ip::address_v4 host;
    bool connected{};
  };
  parallel = std::max<std::size_t>(parallel, 1U);
  for (std::size_t first = 0U; first < hosts.size() && !stop;
       first += parallel) {
    std::vector<std::unique_ptr<Probe>> probes;
    const std::size_t last = std::min(hosts.size(), first + parallel);
    for (std::size_t index = first; index < last; ++index) {
      auto probe = std::make_unique<Probe>(io);
      probe->host = hosts[index];
      Probe *const raw = probe.get();
      raw->socket.async_connect(
          asio::ip::tcp::endpoint{raw->host, port},
          [raw](const std::error_code &error) {
            raw->connected = !error;
            raw->timer.cancel();
          });
      raw->timer.expires_after(timeout);
      raw->timer.async_wait([raw](const std::error_code &error) {
        if (!error) {
          std::error_code ignored;
          raw->socket.close(ignored);
        }
      });
      probes.push_back(std::move(probe));
    }
    io.restart();
    io.run();
    for (const auto &probe : probes) {
      if (probe->connected) {
        open.push_back(probe->host);
      }
      std::error_code ignored;
      probe->socket.close(ignored);
    }
  }
  return open;
}

} // namespace

struct ProcessorMonitor::State final {
  explicit State(ProcessorMonitorConfig value) : config(std::move(value)) {}

  ProcessorMonitorConfig config;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::thread thread;
  std::thread search_thread;
  std::atomic_bool stop{};
  bool started{};

  std::vector<ProcessorTarget> targets;
  std::map<doc::NodeId, Watched> watched;
  std::vector<ProcessorEdidWrite> writes;
  ProcessorSearchState search;

  void loop() {
    std::unique_lock lock{mutex};
    while (!stop) {
      // Bring the watch list up to date.
      for (auto entry = watched.begin(); entry != watched.end();) {
        const bool kept = std::any_of(
            targets.begin(), targets.end(), [&](const ProcessorTarget &t) {
              return t == entry->second.target;
            });
        entry = kept ? std::next(entry) : watched.erase(entry);
      }
      for (const ProcessorTarget &target : targets) {
        if (!watched.contains(target.device)) {
          Watched fresh{};
          fresh.target = target;
          fresh.node.device = target.device;
          fresh.node.reachability =
              topology::HardwareRuntimeReachability::Probing;
          fresh.due = Clock::now();
          watched.emplace(target.device, std::move(fresh));
        }
      }

      if (!writes.empty()) {
        const ProcessorEdidWrite write = writes.front();
        writes.erase(writes.begin());
        apply_write(lock, write);
        continue;
      }

      Clock::time_point next = Clock::now() + config.poll_interval;
      std::optional<doc::NodeId> due_device;
      for (const auto &[device, entry] : watched) {
        if (entry.due <= Clock::now()) {
          due_device = device;
          break;
        }
        next = std::min(next, entry.due);
      }
      if (!due_device.has_value()) {
        wake.wait_until(lock, next);
        continue;
      }
      poll_one(lock, *due_device);
    }
  }

  // Polls one processor with the lock released for the network round trip.
  void poll_one(std::unique_lock<std::mutex> &lock, doc::NodeId device) {
    const auto found = watched.find(device);
    if (found == watched.end()) {
      return;
    }
    const ProcessorTarget target = found->second.target;
    found->second.due = Clock::now() + config.poll_interval;
    lock.unlock();
    std::optional<coex::Poll> result;
    std::string refused;
    const std::optional<HttpEndpoint> endpoint =
        parse_http_endpoint(target.address, coex::default_port);
    if (!endpoint.has_value()) {
      refused = target.address.empty() ? "no address"
                                       : "address is not a numeric IP";
    } else {
      try {
        result = coex::poll(*endpoint, config.request_timeout);
      } catch (const std::exception &error) {
        refused = error.what();
      }
    }
    lock.lock();
    const auto entry = watched.find(device);
    if (entry == watched.end() || !(entry->second.target == target)) {
      return;
    }
    Watched &watch = entry->second;
    if (!result.has_value()) {
      watch.node.reachability =
          topology::HardwareRuntimeReachability::ConfigurationError;
      watch.node.detail = refused;
      watch.node.inputs.clear();
      watch.node.screens.clear();
      return;
    }
    if (result->node.reachability ==
        topology::HardwareRuntimeReachability::Offline) {
      // Keep the last good report, marked as history.
      watch.node.reachability = topology::HardwareRuntimeReachability::Offline;
      watch.node.stale = watch.ever_reached;
      watch.node.detail = result->node.detail;
      return;
    }
    std::string notice = std::move(watch.node.notice);
    const bool notice_failed = watch.node.notice_failed;
    watch.node = std::move(result->node);
    watch.node.device = device;
    watch.node.notice = std::move(notice);
    watch.node.notice_failed = notice_failed;
    watch.inputs = std::move(result->inputs);
    watch.ever_reached = true;
  }

  void apply_write(std::unique_lock<std::mutex> &lock,
                   const ProcessorEdidWrite &write) {
    const auto found = watched.find(write.device);
    if (found == watched.end()) {
      return;
    }
    Watched &watch = found->second;
    const topology::HardwareProcessorInput *const input =
        watch.node.input(write.input);
    const std::string mode = topology::format_video_mode(
        write.width, write.height, write.refresh_millihertz);
    if (input == nullptr) {
      watch.node.notice = "Input " + std::to_string(write.input) +
                          " has not been reported by the processor";
      watch.node.notice_failed = true;
      return;
    }
    const core::i64 vendor_id = input->vendor_id;
    const ProcessorTarget target = watch.target;
    lock.unlock();
    coex::WriteResult result{};
    if (const auto endpoint =
            parse_http_endpoint(target.address, coex::default_port)) {
      try {
        result = coex::write_edid(*endpoint, vendor_id, write.width,
                                  write.height, write.refresh_millihertz,
                                  config.request_timeout);
      } catch (const std::exception &error) {
        result = coex::WriteResult{false, error.what()};
      }
    } else {
      result = coex::WriteResult{false, "address is not a numeric IP"};
    }
    lock.lock();
    const auto entry = watched.find(write.device);
    if (entry == watched.end()) {
      return;
    }
    entry->second.node.notice =
        result.ok ? "Input " + std::to_string(write.input) +
                        " EDID set to " + mode
                  : "Could not set input " + std::to_string(write.input) +
                        " EDID: " + result.message;
    entry->second.node.notice_failed = !result.ok;
    // Read it back now rather than at the next interval.
    entry->second.due = Clock::now();
  }

  void run_search() {
    std::vector<topology::HardwareDiscoveredProcessor> found;
    try {
      const std::vector<asio::ip::address_v4> hosts = search_hosts(config);
      const std::vector<asio::ip::address_v4> open =
          open_hosts(hosts, config.search_port, config.search_connect_timeout,
                     config.search_parallel, stop);
      for (const asio::ip::address_v4 &host : open) {
        if (stop) {
          break;
        }
        const HttpEndpoint endpoint{host.to_string(), config.search_port};
        if (auto processor = coex::identify(endpoint, config.request_timeout)) {
          found.push_back(std::move(*processor));
        }
      }
    } catch (const std::exception &) {
      // A search that fails part way reports what it found.
    }
    std::sort(found.begin(), found.end(),
              [](const auto &left, const auto &right) {
                return left.address < right.address;
              });
    std::lock_guard guard{mutex};
    search.found = std::move(found);
    search.running = false;
    ++search.completed;
  }
};

ProcessorMonitor::ProcessorMonitor(ProcessorMonitorConfig config)
    : state_(std::make_unique<State>(std::move(config))) {}

ProcessorMonitor::~ProcessorMonitor() { stop(); }

void ProcessorMonitor::start() {
  std::lock_guard guard{state_->mutex};
  if (state_->started) {
    return;
  }
  state_->started = true;
  state_->stop = false;
  state_->thread = std::thread{[state = state_.get()] { state->loop(); }};
}

void ProcessorMonitor::stop() noexcept {
  {
    std::lock_guard guard{state_->mutex};
    state_->stop = true;
  }
  state_->wake.notify_all();
  if (state_->thread.joinable()) {
    state_->thread.join();
  }
  if (state_->search_thread.joinable()) {
    state_->search_thread.join();
  }
  std::lock_guard guard{state_->mutex};
  state_->started = false;
}

void ProcessorMonitor::watch(std::vector<ProcessorTarget> targets) {
  std::lock_guard guard{state_->mutex};
  if (targets == state_->targets) {
    return;
  }
  state_->targets = std::move(targets);
  state_->wake.notify_all();
}

std::vector<topology::HardwareProcessorNode>
ProcessorMonitor::processors() const {
  std::vector<topology::HardwareProcessorNode> nodes;
  std::lock_guard guard{state_->mutex};
  nodes.reserve(state_->targets.size());
  for (const ProcessorTarget &target : state_->targets) {
    const auto found = state_->watched.find(target.device);
    if (found != state_->watched.end()) {
      nodes.push_back(found->second.node);
    } else {
      topology::HardwareProcessorNode waiting{};
      waiting.device = target.device;
      waiting.reachability = topology::HardwareRuntimeReachability::Probing;
      nodes.push_back(std::move(waiting));
    }
  }
  return nodes;
}

void ProcessorMonitor::search() {
  std::lock_guard guard{state_->mutex};
  if (state_->search.running || state_->stop) {
    return;
  }
  if (state_->search_thread.joinable()) {
    state_->search_thread.join();
  }
  state_->search.running = true;
  state_->search_thread =
      std::thread{[state = state_.get()] { state->run_search(); }};
}

ProcessorSearchState ProcessorMonitor::search_state() const {
  std::lock_guard guard{state_->mutex};
  return state_->search;
}

void ProcessorMonitor::write_edid(const ProcessorEdidWrite &write) {
  {
    std::lock_guard guard{state_->mutex};
    state_->writes.push_back(write);
  }
  state_->wake.notify_all();
}

std::vector<ProcessorTarget>
processor_targets(const topology::HardwareMap &map) {
  std::vector<ProcessorTarget> targets;
  for (const topology::HardwareDevice &device : map.devices) {
    // A processor with no address has nothing to ask; its card says so.
    if (device.kind != topology::HardwareDeviceKind::LedProcessor ||
        device.protocol == topology::HardwareProcessorProtocol::Generic ||
        device.address.empty()) {
      continue;
    }
    targets.push_back(ProcessorTarget{device.node, device.protocol,
                                      device.address});
  }
  return targets;
}

} // namespace mediaserver::devices
