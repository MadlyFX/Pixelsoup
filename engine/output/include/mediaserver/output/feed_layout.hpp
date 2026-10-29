#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#include <string>
#include <vector>

// A starting Feed Map built from the hardware itself.
//
// The Hardware Map knows which render output is cabled to which processor
// input, and each output's window raster. A processor that reports its
// screens adds the rest: how big each LED screen is, and which part of which
// input lands where on it. Together that is a whole layout - a canvas with
// every screen laid side by side, and in each window the viewport the
// processor actually shows, cropping the canvas pixels that belong there.
//
// A processor that reports nothing still gets a usable start: its windows,
// side by side in input order, each shown whole. The plan says so in its
// notes rather than inventing a screen layout.
//
// This is pure: the same map and observations always make the same plan.
namespace mediaserver::output {

struct FeedLayoutViewport final {
  // The window (feed index) the viewport is drawn in.
  core::u32 window{};
  std::string name;
  // The canvas pixels it shows.
  scene::CanvasRect source{};
  // Where they land in the window.
  scene::CanvasRect viewport{};

  bool operator==(const FeedLayoutViewport &) const = default;
};

// One LED screen's place in the canvas.
struct FeedLayoutScreen final {
  std::string name;
  scene::CanvasRect canvas{};
  // Its size came from the processor rather than from its windows.
  bool reported{};

  bool operator==(const FeedLayoutScreen &) const = default;
};

struct FeedLayoutPlan final {
  scene::Canvas canvas{};
  std::vector<FeedLayoutViewport> viewports;
  std::vector<FeedLayoutScreen> screens;
  // Every window the plan lays out; their current viewports are replaced.
  std::vector<core::u32> windows;
  // What the operator should know about how the plan was made.
  std::vector<std::string> notes;

  [[nodiscard]] bool empty() const noexcept { return viewports.empty(); }
  bool operator==(const FeedLayoutPlan &) const = default;
};

[[nodiscard]] FeedLayoutPlan
plan_feed_layout(const topology::HardwareMap &map,
                 const topology::HardwareRuntimeSnapshot &runtime);

} // namespace mediaserver::output
