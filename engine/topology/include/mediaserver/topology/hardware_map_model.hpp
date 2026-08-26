#pragma once

#include <mediaserver/doc/document.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <map>

namespace mediaserver::topology {

struct HardwareMapCounters final {
    // Cumulative parse failures since construction/reset.
    core::u64 rejected{};
    // Currently parsed document entries, before capacity truncation.
    core::u32 devices{};
    core::u32 links{};
    // Currently parsed links omitted by evaluate because an endpoint is
    // missing, a port is invalid/incompatible, or a lower-id link already owns
    // the same input.
    core::u32 invalid_links{};

    bool operator==(const HardwareMapCounters&) const = default;

    [[nodiscard]] core::u32 devices_over_capacity() const noexcept;
    [[nodiscard]] core::u32 links_over_capacity() const noexcept;
};

class HardwareMapModel final {
  public:
    HardwareMapModel() = default;

    void reset(const doc::Root& root);
    void apply(const doc::Delta& delta);

    [[nodiscard]] HardwareMap evaluate() const;
    [[nodiscard]] std::size_t size() const noexcept {
        return devices_.size() + links_.size() + settings_.size();
    }
    [[nodiscard]] const HardwareMapCounters& counters() const noexcept {
        return counters_;
    }

  private:
    std::map<doc::NodeId, HardwareDevice> devices_;
    std::map<doc::NodeId, HardwareLink> links_;
    std::map<doc::NodeId, HardwareControlSettings> settings_;
    HardwareMapCounters counters_{};

    void put(const doc::Node& node);
    void forget(doc::NodeId id) noexcept;
    void recount() noexcept;
};

} // namespace mediaserver::topology
