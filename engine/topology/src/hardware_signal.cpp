#include <mediaserver/topology/hardware_signal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>

namespace mediaserver::topology {
namespace {

[[nodiscard]] bool same_rate(core::u32 left, core::u32 right) noexcept {
  const core::u32 difference = left > right ? left - right : right - left;
  return difference <= refresh_tolerance_millihertz;
}

[[nodiscard]] bool processor_reachable(const HardwareProcessorNode &node) noexcept {
  return !node.stale &&
         node.reachability == HardwareRuntimeReachability::Online;
}

} // namespace

core::u32 refresh_millihertz(core::u32 numerator,
                             core::u32 denominator) noexcept {
  if (denominator == 0U) {
    return 0U;
  }
  const core::u64 scaled = static_cast<core::u64>(numerator) * 1000U;
  return static_cast<core::u32>((scaled + denominator / 2U) / denominator);
}

HardwareRefresh refresh_from_millihertz(core::u32 millihertz) noexcept {
  if (millihertz == 0U) {
    return HardwareRefresh{};
  }
  // n*1000/1001 Hz is n*1000*1000/1001 millihertz: try the whole rate n
  // that would make it, then the whole rate itself.
  const core::u64 whole =
      (static_cast<core::u64>(millihertz) * 1001U + 500'000U) / 1'000'000U;
  if (whole != 0U && whole <= 1'000U) {
    const auto ntsc = static_cast<core::u32>(whole * 1000U);
    if (same_rate(refresh_millihertz(ntsc, 1001U), millihertz) &&
        !same_rate(ntsc, millihertz)) {
      return HardwareRefresh{ntsc, 1001U};
    }
  }
  const core::u32 hertz = (millihertz + 500U) / 1000U;
  if (hertz != 0U && same_rate(hertz * 1000U, millihertz)) {
    return HardwareRefresh{hertz, 1U};
  }
  return HardwareRefresh{millihertz, 1000U};
}

HardwareRefresh refresh_from_hertz(double hertz) noexcept {
  if (!(hertz > 0.0)) {
    return HardwareRefresh{};
  }
  const double held = std::min(hertz, max_refresh_hertz);
  const auto millihertz =
      static_cast<core::u32>(std::max(std::llround(held * 1000.0), 1LL));
  return refresh_from_millihertz(millihertz);
}

int refresh_decimals(core::u32 millihertz) noexcept {
  if (millihertz % 1000U == 0U) {
    return 0;
  }
  if (millihertz % 100U == 0U) {
    return 1;
  }
  return millihertz % 10U == 0U ? 2 : 3;
}

HardwareOutputMode output_mode_for(const HardwareMap &map,
                                   const HardwareDevice &render_server,
                                   core::u8 output_port) noexcept {
  if (const HardwareOutputBinding *const binding =
          map.output_binding_for(render_server.node, output_port)) {
    return HardwareOutputMode{binding->width, binding->height,
                              binding->refresh_numerator,
                              binding->refresh_denominator};
  }
  return HardwareOutputMode{render_server.width, render_server.height,
                            render_server.refresh_numerator,
                            render_server.refresh_denominator};
}

HardwareLinkRegion link_output_region(const HardwareMap &map,
                                      const HardwareLink &link,
                                      const HardwareRuntimeSnapshot &runtime) {
  const HardwareDevice *const from = map.device(link.from.device);
  if (from == nullptr || from->kind != HardwareDeviceKind::RenderServer ||
      link.from.port < first_render_server_video_port ||
      link.from.port > last_render_server_video_port) {
    return HardwareLinkRegion{{}, HardwareRegionSource::Authored};
  }
  const HardwareOutputMode mode = output_mode_for(map, *from, link.from.port);
  const HardwareOutputRegion whole{0U, 0U, mode.width, mode.height};
  if (!link.region.empty()) {
    HardwareOutputRegion cut = link.region;
    cut.x = std::min(cut.x, whole.width);
    cut.y = std::min(cut.y, whole.height);
    cut.width = std::min(cut.width, whole.width - cut.x);
    cut.height = std::min(cut.height, whole.height - cut.y);
    return HardwareLinkRegion{cut, HardwareRegionSource::Authored};
  }

  std::vector<const HardwareLink *> siblings;
  for (const HardwareLink &other : map.links) {
    if (other.from == link.from) {
      siblings.push_back(&other);
    }
  }
  if (siblings.size() <= 1U) {
    return HardwareLinkRegion{whole, HardwareRegionSource::WholeOutput};
  }
  const auto order = [&](const HardwareLink *cable) {
    const HardwareDevice *const to = map.device(cable->to.device);
    const core::i64 x = to != nullptr ? static_cast<core::i64>(to->grid_x) : 0;
    const core::i64 y = to != nullptr ? static_cast<core::i64>(to->grid_y) : 0;
    return std::tuple{x - y, x + y, processor_input_number(cable->to.port),
                      cable->node.value()};
  };
  std::stable_sort(siblings.begin(), siblings.end(),
                   [&](const HardwareLink *left, const HardwareLink *right) {
                     return order(left) < order(right);
                   });
  const std::size_t index = static_cast<std::size_t>(
      std::find(siblings.begin(), siblings.end(), &link) - siblings.begin());
  if (index >= siblings.size()) {
    return HardwareLinkRegion{whole, HardwareRegionSource::WholeOutput};
  }

  if (const HardwareOutputBinding *const binding =
          map.output_binding_for(from->node, link.from.port)) {
    if (const HardwareRuntimeNode *const node = runtime.node(from->node)) {
      for (const HardwareVideoOutput &output : node->video_outputs) {
        if (output.target == binding->target &&
            index < output.mosaic_tiles.size()) {
          return HardwareLinkRegion{output.mosaic_tiles[index],
                                    HardwareRegionSource::MosaicTile};
        }
      }
    }
  }

  const core::u64 count = siblings.size();
  const core::u32 left =
      static_cast<core::u32>(whole.width * static_cast<core::u64>(index) / count);
  const core::u32 right = static_cast<core::u32>(
      whole.width * static_cast<core::u64>(index + 1U) / count);
  return HardwareLinkRegion{
      HardwareOutputRegion{left, 0U, right - left, whole.height},
      HardwareRegionSource::EvenShare};
}

std::vector<HardwareSignalCheck>
check_processor_signals(const HardwareMap &map,
                        const HardwareRuntimeSnapshot &runtime) {
  std::vector<HardwareSignalCheck> checks;
  for (const HardwareLink &link : map.links) {
    const HardwareDevice *const from = map.device(link.from.device);
    const HardwareDevice *const to = map.device(link.to.device);
    if (from == nullptr || to == nullptr ||
        from->kind != HardwareDeviceKind::RenderServer ||
        to->kind != HardwareDeviceKind::LedProcessor ||
        to->protocol == HardwareProcessorProtocol::Generic) {
      continue;
    }
    const core::u8 number = processor_input_number(link.to.port);
    const HardwareProcessorNode *const observed = runtime.processor(to->node);
    if (number == 0U || observed == nullptr || !processor_reachable(*observed)) {
      continue;
    }
    const HardwareProcessorInput *const input = observed->input(number);
    if (input == nullptr) {
      continue;
    }

    const HardwareOutputMode mode =
        output_mode_for(map, *from, link.from.port);
    HardwareSignalCheck check{};
    check.link = link.node;
    check.render_server = from->node;
    check.output_port = link.from.port;
    check.processor = to->node;
    check.input = number;
    const HardwareOutputRegion carried =
        link_output_region(map, link, runtime).region;
    check.width = carried.width;
    check.height = carried.height;
    check.refresh_millihertz =
        refresh_millihertz(mode.refresh_numerator, mode.refresh_denominator);

    const bool edid_known = input->edid_width != 0U && input->edid_height != 0U;
    const bool edid_differs =
        edid_known &&
        (input->edid_width != check.width ||
         input->edid_height != check.height ||
         (input->edid_refresh_millihertz != 0U &&
          !same_rate(input->edid_refresh_millihertz, check.refresh_millihertz)));
    const bool mode_differs =
        input->signal &&
        (input->width != check.width || input->height != check.height ||
         (input->refresh_millihertz != 0U &&
          !same_rate(input->refresh_millihertz, check.refresh_millihertz)));

    if (edid_differs) {
      check.issue = HardwareSignalIssue::EdidMismatch;
      check.seen_width = input->edid_width;
      check.seen_height = input->edid_height;
      check.seen_refresh_millihertz = input->edid_refresh_millihertz;
      check.edid_writable = input->edid_writable;
    } else if (!input->signal) {
      check.issue = HardwareSignalIssue::NoSignal;
    } else if (mode_differs) {
      check.issue = HardwareSignalIssue::ModeMismatch;
      check.seen_width = input->width;
      check.seen_height = input->height;
      check.seen_refresh_millihertz = input->refresh_millihertz;
    }
    checks.push_back(check);
  }
  return checks;
}

std::string format_video_mode(core::u32 width, core::u32 height,
                              core::u32 refresh_millihertz) {
  std::array<char, 64U> text{};
  int length{};
  if (refresh_millihertz == 0U) {
    length = std::snprintf(text.data(), text.size(), "%ux%u", width, height);
  } else if (refresh_millihertz % 1000U == 0U) {
    length = std::snprintf(text.data(), text.size(), "%ux%u at %u", width,
                           height, refresh_millihertz / 1000U);
  } else {
    length = std::snprintf(text.data(), text.size(), "%ux%u at %.2f", width,
                           height,
                           static_cast<double>(refresh_millihertz) / 1000.0);
  }
  return std::string{text.data(),
                     static_cast<std::size_t>(length > 0 ? length : 0)};
}

std::string signal_check_text(const HardwareSignalCheck &check) {
  const std::string wanted =
      format_video_mode(check.width, check.height, check.refresh_millihertz);
  const std::string seen = format_video_mode(
      check.seen_width, check.seen_height, check.seen_refresh_millihertz);
  const std::string input = "input " + std::to_string(check.input);
  switch (check.issue) {
  case HardwareSignalIssue::None:
    return {};
  case HardwareSignalIssue::NoSignal:
    return "No signal at processor " + input;
  case HardwareSignalIssue::EdidMismatch:
    return "EDID mismatch: processor " + input + " offers " + seen +
           ", output sends " + wanted;
  case HardwareSignalIssue::ModeMismatch:
    return "Processor " + input + " receives " + seen + ", output is set to " +
           wanted;
  }
  return {};
}

} // namespace mediaserver::topology
