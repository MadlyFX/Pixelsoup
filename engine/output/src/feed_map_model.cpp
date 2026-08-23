#include <mediaserver/output/feed_map_model.hpp>

#include <mediaserver/output/feed_map_schema.hpp>

#include <algorithm>
#include <cstdint>

namespace mediaserver::output {
namespace {

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties,
    std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

// A property reader with the same policy as the scene model's: an absent
// property takes its default, a property present with the wrong type rejects
// the whole node. A feed map is all whole numbers, so this carries one getter.
class Reader final {
public:
    explicit Reader(const doc::PropertyMap& properties) noexcept
        : properties_(&properties) {}

    [[nodiscard]] std::int64_t integer(
        std::string_view name,
        std::int64_t fallback) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            return *whole;
        }
        ok_ = false;
        return fallback;
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }

private:
    const doc::PropertyMap* properties_;
    bool ok_{true};
};

// An integer property that has to fit an unsigned 32-bit field. Rejects rather
// than clamps: a negative width is not a small width.
[[nodiscard]] bool as_u32(std::int64_t value, core::u32& into) noexcept {
    if (value < 0 || value > static_cast<std::int64_t>(UINT32_MAX)) {
        return false;
    }
    into = static_cast<core::u32>(value);
    return true;
}

} // namespace

core::u32 FeedMapCounters::over_capacity() const noexcept {
    return maps > max_output_maps ? maps - max_output_maps : 0U;
}

void FeedMapModel::forget(doc::NodeId id) noexcept {
    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        return;
    }
    if (counters_.maps != 0U) {
        --counters_.maps;
    }
    entries_.erase(found);
}

void FeedMapModel::put(const doc::Node& node) {
    // A node that stops being a feed-map node has to stop being a binding, so
    // the old entry goes first whatever the new type turns out to be.
    forget(node.id);

    if (node.type != schema::map_type) {
        // Not a feed-map node. Nothing to hold and nothing to report.
        return;
    }

    Reader reader{node.properties};

    core::u32 index{};
    core::u32 x{};
    core::u32 y{};
    core::u32 width{};
    core::u32 height{};
    if (!as_u32(reader.integer(schema::node_index, 0), index)
        || !as_u32(reader.integer(schema::source_x, 0), x)
        || !as_u32(reader.integer(schema::source_y, 0), y)
        || !as_u32(reader.integer(schema::source_width, 0), width)
        || !as_u32(reader.integer(schema::source_height, 0), height)) {
        reader.fail();
    }
    if (width == 0U || height == 0U) {
        reader.fail();
    }

    if (!reader.ok()) {
        ++counters_.rejected;
        return;
    }

    OutputMap map{};
    map.node_index = index;
    map.source = scene::CanvasRect{x, y, width, height};
    ++counters_.maps;
    entries_.insert_or_assign(node.id, map);
}

void FeedMapModel::reset(const doc::Root& root) {
    entries_.clear();
    counters_ = FeedMapCounters{};
    // Diffing against an empty root enumerates every node in ascending id
    // order - the same code path a delta takes, so a checkpointed node and a
    // streamed one cannot parse differently.
    apply(diff(doc::Root{}, root));
}

void FeedMapModel::apply(const doc::Delta& delta) {
    for (const auto& change : delta.changes) {
        switch (change.kind) {
        case doc::ChangeKind::Removed:
            forget(change.id);
            break;
        case doc::ChangeKind::Added:
        case doc::ChangeKind::Modified:
            if (change.after) {
                put(*change.after);
            }
            break;
        }
    }
}

FeedMap FeedMapModel::evaluate() const {
    FeedMap feed_map{};

    // Entries are in node id order, so the first binding seen for a node index
    // is the lowest-id one and wins. Truncation at the cap is by node id too,
    // so every node keeps the same maps.
    for (const auto& [id, map] : entries_) {
        static_cast<void>(id);
        if (feed_map.maps.size() >= max_output_maps) {
            break;
        }
        const auto already = std::any_of(
            feed_map.maps.begin(),
            feed_map.maps.end(),
            [&map](const OutputMap& held) noexcept {
                return held.node_index == map.node_index;
            });
        if (already) {
            continue;
        }
        feed_map.maps.push_back(map);
    }

    // By node index, so the order the renderer and the digest see is a property
    // of the document rather than of node ids.
    std::sort(
        feed_map.maps.begin(),
        feed_map.maps.end(),
        [](const OutputMap& left, const OutputMap& right) noexcept {
            return left.node_index < right.node_index;
        });

    return feed_map;
}

} // namespace mediaserver::output
