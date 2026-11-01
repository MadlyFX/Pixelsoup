#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#include <string>
#include <vector>

// What the cable between a render output and an LED processor input carries,
// checked against what the processor says it receives.
//
// The render output's mode is the authored one: its output binding's raster
// and rate, or the Render Server's own for a card without bindings. The
// processor's side is an observation, so nothing here is document data and a
// processor that is not monitored is simply not checked.
namespace mediaserver::topology {

enum class HardwareSignalIssue : core::u8 {
  // Nothing to report: the input receives the output's mode, or the
  // processor cannot be asked.
  None,
  // The processor sees nothing on that input.
  NoSignal,
  // The input's EDID offers a different mode, so the render machine's GPU
  // is not offered the one the output is set to.
  EdidMismatch,
  // A signal arrives, in a different mode from the output's.
  ModeMismatch,
};

// A refresh rate in thousandths of a hertz: 60000/1001 is 59940.
[[nodiscard]] core::u32 refresh_millihertz(core::u32 numerator,
                                           core::u32 denominator) noexcept;

// Rates within this of each other are the same rate: a processor reports
// 59.94 for 60000/1001, never 59.9400599.
inline constexpr core::u32 refresh_tolerance_millihertz = 10U;

// A rate in thousandths of a hertz as an exact fraction: the NTSC family
// (23.976, 29.97, 59.94...) becomes n*1000/1001, a whole rate n/1, and
// anything else stays in thousandths. Zero gives 0/1.
struct HardwareRefresh final {
  core::u32 numerator{};
  core::u32 denominator{1U};

  bool operator==(const HardwareRefresh &) const = default;
};
[[nodiscard]] HardwareRefresh
refresh_from_millihertz(core::u32 millihertz) noexcept;

// The highest rate an operator may type, in hertz.
inline constexpr double max_refresh_hertz = 1'000.0;

// A rate typed in hertz (23.976, 59.94, 50) as the same exact fraction,
// held to 0.001..max_refresh_hertz; a value that is not a number is 0/1.
[[nodiscard]] HardwareRefresh refresh_from_hertz(double hertz) noexcept;

// How many decimals show a rate as typed: 60, 59.94, 23.976.
[[nodiscard]] int refresh_decimals(core::u32 millihertz) noexcept;

struct HardwareSignalCheck final {
  doc::NodeId link{};
  doc::NodeId render_server{};
  core::u8 output_port{};
  doc::NodeId processor{};
  // The processor's VIDEO IN number.
  core::u8 input{};
  // The most useful thing to tell the operator. An EDID mismatch outranks
  // the mode mismatch it causes, since fixing it fixes both.
  HardwareSignalIssue issue{HardwareSignalIssue::None};
  // What the output is set to send down this cable: its region of the
  // output, at the output's rate.
  core::u32 width{};
  core::u32 height{};
  core::u32 refresh_millihertz{};
  // What the processor reports for the issue: the EDID's mode for an EDID
  // mismatch, the received mode for a mode mismatch.
  core::u32 seen_width{};
  core::u32 seen_height{};
  core::u32 seen_refresh_millihertz{};
  // An EDID mismatch the processor can be told to correct.
  bool edid_writable{};

  bool operator==(const HardwareSignalCheck &) const = default;
};

// One check per cable from a Render Server video output to a monitored LED
// processor's input whose processor has reported that input, in link order.
[[nodiscard]] std::vector<HardwareSignalCheck>
check_processor_signals(const HardwareMap &map,
                        const HardwareRuntimeSnapshot &runtime);

// The mode an output is set to send: its binding when it has one, otherwise
// the Render Server card's own raster and rate.
struct HardwareOutputMode final {
  core::u32 width{};
  core::u32 height{};
  core::u32 refresh_numerator{};
  core::u32 refresh_denominator{1U};

  bool operator==(const HardwareOutputMode &) const = default;
};

[[nodiscard]] HardwareOutputMode
output_mode_for(const HardwareMap &map, const HardwareDevice &render_server,
                core::u8 output_port) noexcept;

// Where an unauthored cable's region came from.
enum class HardwareRegionSource : core::u8 {
  // The cable's own authored region, cut to the output.
  Authored,
  // The output's only cable carries all of it.
  WholeOutput,
  // The tile of an NVIDIA Mosaic surface the render machine reported.
  MosaicTile,
  // An even left-to-right share among the output's cables.
  EvenShare,
};

struct HardwareLinkRegion final {
  HardwareOutputRegion region{};
  HardwareRegionSource source{HardwareRegionSource::WholeOutput};

  bool operator==(const HardwareLinkRegion &) const = default;
};

// The part of its Render Server output a cable carries. An authored region
// wins. Otherwise the output's cables, ordered by where their targets sit on
// the map (left to right) and then by input, take the Mosaic tiles the
// render machine reports for the output's bound display in row order, or
// else equal slices of its width. A cable from anything other than a Render
// Server video output carries nothing (an empty region).
[[nodiscard]] HardwareLinkRegion
link_output_region(const HardwareMap &map, const HardwareLink &link,
                   const HardwareRuntimeSnapshot &runtime);

// "3840x2160 at 59.94", for a check's text.
[[nodiscard]] std::string format_video_mode(core::u32 width, core::u32 height,
                                            core::u32 refresh_millihertz);

// One sentence for the operator, or empty for HardwareSignalIssue::None.
[[nodiscard]] std::string signal_check_text(const HardwareSignalCheck &check);

} // namespace mediaserver::topology
