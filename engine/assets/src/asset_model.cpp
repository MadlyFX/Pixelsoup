#include <mediaserver/assets/asset_model.hpp>

#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/content_resolver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace mediaserver::assets {
namespace {

[[nodiscard]] const doc::PropertyValue* find(const doc::PropertyMap& properties,
                                             std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

class Reader final {
  public:
    explicit Reader(const doc::PropertyMap& properties) noexcept
        : properties_(&properties) {}

    [[nodiscard]] bool has(std::string_view name) const noexcept {
        return find(*properties_, name) != nullptr;
    }

    [[nodiscard]] std::string text(std::string_view name,
                                   std::string_view fallback) noexcept {
        const doc::PropertyValue* const value = find(*properties_, name);
        if (value == nullptr) {
            return std::string{fallback};
        }
        if (const auto* const held = std::get_if<std::string>(value)) {
            return *held;
        }
        ok_ = false;
        return std::string{fallback};
    }

    [[nodiscard]] std::optional<std::int64_t>
    optional_integer(std::string_view name) noexcept {
        const doc::PropertyValue* const value = find(*properties_, name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (const auto* const held = std::get_if<std::int64_t>(value)) {
            return *held;
        }
        ok_ = false;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double>
    optional_number(std::string_view name) noexcept {
        const doc::PropertyValue* const value = find(*properties_, name);
        if (value == nullptr) {
            return std::nullopt;
        }
        double result{};
        if (const auto* const real = std::get_if<double>(value)) {
            result = *real;
        } else if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            result = static_cast<double>(*whole);
        } else {
            ok_ = false;
            return std::nullopt;
        }
        if (!std::isfinite(result)) {
            ok_ = false;
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }

  private:
    const doc::PropertyMap* properties_{};
    bool ok_{true};
};

[[nodiscard]] std::optional<core::u32>
read_u32(Reader& reader, std::string_view name, bool allow_zero) noexcept {
    const std::optional<std::int64_t> value = reader.optional_integer(name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    const std::int64_t minimum = allow_zero ? 0 : 1;
    if (*value < minimum ||
        *value >
            static_cast<std::int64_t>(std::numeric_limits<core::u32>::max())) {
        reader.fail();
        return std::nullopt;
    }
    return static_cast<core::u32>(*value);
}

[[nodiscard]] std::optional<core::u64>
read_positive_u64(Reader& reader, std::string_view name) noexcept {
    const std::optional<std::int64_t> value = reader.optional_integer(name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value <= 0) {
        reader.fail();
        return std::nullopt;
    }
    return static_cast<core::u64>(*value);
}

[[nodiscard]] std::optional<ModelBounds> read_bounds(Reader& reader) noexcept {
    constexpr std::array<std::string_view, 6U> properties{{
        schema::bounds_minimum_x,
        schema::bounds_minimum_y,
        schema::bounds_minimum_z,
        schema::bounds_maximum_x,
        schema::bounds_maximum_y,
        schema::bounds_maximum_z,
    }};
    bool any = false;
    bool all = true;
    for (const std::string_view property : properties) {
        any = any || reader.has(property);
        all = all && reader.has(property);
    }
    if (!any) {
        return std::nullopt;
    }
    if (!all) {
        reader.fail();
        return std::nullopt;
    }

    ModelBounds bounds{};
    const auto minimum_x = reader.optional_number(schema::bounds_minimum_x);
    const auto minimum_y = reader.optional_number(schema::bounds_minimum_y);
    const auto minimum_z = reader.optional_number(schema::bounds_minimum_z);
    const auto maximum_x = reader.optional_number(schema::bounds_maximum_x);
    const auto maximum_y = reader.optional_number(schema::bounds_maximum_y);
    const auto maximum_z = reader.optional_number(schema::bounds_maximum_z);
    if (!minimum_x.has_value() || !minimum_y.has_value() ||
        !minimum_z.has_value() || !maximum_x.has_value() ||
        !maximum_y.has_value() || !maximum_z.has_value()) {
        reader.fail();
        return std::nullopt;
    }
    bounds = ModelBounds{*minimum_x, *minimum_y, *minimum_z,
                         *maximum_x, *maximum_y, *maximum_z};
    if (bounds.minimum_x > bounds.maximum_x ||
        bounds.minimum_y > bounds.maximum_y ||
        bounds.minimum_z > bounds.maximum_z) {
        reader.fail();
        return std::nullopt;
    }
    return bounds;
}

[[nodiscard]] std::pair<std::string, std::string>
read_identity(Reader& reader) {
    std::string source = reader.text(schema::source, "");
    std::string display_name = reader.text(schema::display_name, source);
    if (source.empty() || display_name.empty()) {
        reader.fail();
    }
    return {std::move(display_name), std::move(source)};
}

[[nodiscard]] AssetContent read_content(Reader& reader,
                                        std::string_view source) {
    AssetContent content{};
    const std::string policy =
        reader.text(schema::distribution_policy, schema::policy_external);
    if (policy == schema::policy_external) {
        content.policy = AssetDistributionPolicy::External;
    } else if (policy == schema::policy_project_relative) {
        content.policy = AssetDistributionPolicy::ProjectRelative;
    } else if (policy == schema::policy_shared_store) {
        content.policy = AssetDistributionPolicy::SharedStore;
    } else {
        reader.fail();
    }

    const bool has_hash = reader.has(schema::content_sha256);
    const bool has_size = reader.has(schema::content_size_bytes);
    if (has_hash != has_size) {
        reader.fail();
        return content;
    }
    if (has_hash) {
        const std::string hexadecimal = reader.text(schema::content_sha256, "");
        const auto digest = parse_sha256(hexadecimal);
        const auto size =
            read_positive_u64(reader, schema::content_size_bytes);
        if (!digest.has_value() || !size.has_value()) {
            reader.fail();
        } else {
            content.identity = ContentIdentity{*digest, *size};
        }
    }

    if (content.policy != AssetDistributionPolicy::External &&
        !content.identity.has_value()) {
        reader.fail();
    }
    if (content.policy == AssetDistributionPolicy::ProjectRelative &&
        !valid_project_relative_source(source)) {
        reader.fail();
    }
    if (content.policy == AssetDistributionPolicy::SharedStore &&
        !portable_content_extension(source).has_value()) {
        reader.fail();
    }
    return content;
}

} // namespace

core::u32 AssetCounters::over_capacity() const noexcept {
    const core::u64 valid = static_cast<core::u64>(media) + models +
        projection_meshes;
    const core::u64 unique =
        valid > duplicate_sources ? valid - duplicate_sources : 0U;
    return unique > max_project_assets
               ? static_cast<core::u32>(
                     std::min<core::u64>(unique - max_project_assets,
                                         std::numeric_limits<core::u32>::max()))
               : 0U;
}

void AssetModel::forget(doc::NodeId id) noexcept { entries_.erase(id); }

void AssetModel::put(const doc::Node& node) {
    forget(node.id);
    if (node.type != schema::media_type && node.type != schema::model_type &&
        node.type != schema::projection_mesh_type) {
        return;
    }

    Reader reader{node.properties};
    auto [display_name, source] = read_identity(reader);
    const AssetContent content = read_content(reader, source);

    if (node.type == schema::media_type) {
        std::optional<PixelDimensions> dimensions;
        const bool has_width = reader.has(schema::media_width);
        const bool has_height = reader.has(schema::media_height);
        if (has_width != has_height) {
            reader.fail();
        } else if (has_width) {
            const auto width = read_u32(reader, schema::media_width, false);
            const auto height = read_u32(reader, schema::media_height, false);
            if (width.has_value() && height.has_value()) {
                dimensions = PixelDimensions{*width, *height};
            }
        }
        const auto frame_count =
            read_u32(reader, schema::media_frame_count, false);
        std::optional<MediaFrameRate> frame_rate;
        const bool has_rate_numerator = reader.has(schema::media_rate_numerator);
        const bool has_rate_denominator =
            reader.has(schema::media_rate_denominator);
        if (has_rate_numerator != has_rate_denominator) {
            reader.fail();
        } else if (has_rate_numerator) {
            const auto numerator =
                read_u32(reader, schema::media_rate_numerator, false);
            const auto denominator =
                read_u32(reader, schema::media_rate_denominator, false);
            if (numerator.has_value() && denominator.has_value()) {
                frame_rate = MediaFrameRate{*numerator, *denominator};
            }
        }
        if (!reader.ok()) {
            ++counters_.rejected;
            return;
        }
        Asset asset{MediaAsset{node.id, std::move(display_name),
                               std::move(source), dimensions, frame_count,
                               frame_rate, content}};
        if (content.policy == AssetDistributionPolicy::SharedStore &&
            asset_requires_dependency_manifest(asset)) {
            ++counters_.rejected;
            return;
        }
        entries_.insert_or_assign(node.id, std::move(asset));
        return;
    }

    const bool projection_mesh = node.type == schema::projection_mesh_type;
    const auto triangle_count = read_positive_u64(
        reader, projection_mesh ? schema::projection_triangle_count
                                : schema::model_triangle_count);
    const auto material_count = projection_mesh
        ? std::optional<core::u32>{}
        : read_u32(reader, schema::model_material_count, true);
    const auto vertex_count = projection_mesh
        ? read_positive_u64(reader, schema::projection_vertex_count)
        : std::optional<core::u64>{};
    const auto bounds = read_bounds(reader);
    if (!reader.ok()) {
        ++counters_.rejected;
        return;
    }
    Asset asset{ModelAsset{node.id, std::move(display_name), std::move(source),
                           triangle_count, material_count, bounds,
                           vertex_count, projection_mesh, content}};
    if (content.policy == AssetDistributionPolicy::SharedStore &&
        asset_requires_dependency_manifest(asset)) {
        ++counters_.rejected;
        return;
    }
    entries_.insert_or_assign(node.id, std::move(asset));
}

void AssetModel::recount() noexcept {
    core::u32 media = 0U;
    core::u32 models = 0U;
    core::u32 projection_meshes = 0U;
    core::u32 duplicates = 0U;
    std::set<std::pair<AssetKind, std::string_view>> sources;
    for (const auto& [id, asset] : entries_) {
        static_cast<void>(id);
        core::u32& count = std::holds_alternative<MediaAsset>(asset)
            ? media
            : (std::get<ModelAsset>(asset).projection_mesh
                   ? projection_meshes
                   : models);
        if (count != std::numeric_limits<core::u32>::max()) {
            ++count;
        }
        if (!sources.insert({asset_kind(asset), asset_source(asset)}).second &&
            duplicates != std::numeric_limits<core::u32>::max()) {
            ++duplicates;
        }
    }
    counters_.media = media;
    counters_.models = models;
    counters_.projection_meshes = projection_meshes;
    counters_.duplicate_sources = duplicates;
}

void AssetModel::reset(const doc::Root& root) {
    entries_.clear();
    counters_ = AssetCounters{};
    apply(diff(doc::Root{}, root));
}

void AssetModel::apply(const doc::Delta& delta) {
    for (const doc::NodeChange& change : delta.changes) {
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
    recount();
}

AssetRegistry AssetModel::evaluate() const {
    AssetRegistry registry{};
    const core::u64 valid = static_cast<core::u64>(counters_.media) +
        counters_.models + counters_.projection_meshes;
    const core::u64 unique = valid > counters_.duplicate_sources
                                 ? valid - counters_.duplicate_sources
                                 : 0U;
    registry.assets.reserve(static_cast<std::size_t>(
        std::min<core::u64>(unique, max_project_assets)));

    std::set<std::pair<AssetKind, std::string_view>> sources;
    for (const auto& [id, asset] : entries_) {
        static_cast<void>(id);
        if (!sources.insert({asset_kind(asset), asset_source(asset)}).second) {
            continue;
        }
        if (registry.assets.size() >= max_project_assets) {
            break;
        }
        registry.assets.push_back(asset);
    }
    return registry;
}

} // namespace mediaserver::assets
