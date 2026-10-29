#include <mediaserver/topology/hardware_signal.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>

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
    check.width = mode.width;
    check.height = mode.height;
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
