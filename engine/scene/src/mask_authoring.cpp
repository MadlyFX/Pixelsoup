#include <mediaserver/scene/mask_authoring.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::scene {

namespace mask_authoring_detail {

inline constexpr core::u64 max_reference_id =
    static_cast<core::u64>(std::numeric_limits<core::i64>::max());

[[nodiscard]] inline MaskAuthoringError error(
    MaskAuthoringErrorCode code, doc::NodeId subject = {},
    doc::NodeId related = {}) noexcept {
    return MaskAuthoringError{code, subject, related};
}

[[nodiscard]] inline const doc::PropertyValue* find(
    const doc::PropertyMap& properties, std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

[[nodiscard]] inline bool read_reference(
    const doc::Node& node, std::string_view property,
    doc::NodeId& value) noexcept {
    const auto* const held = find(node.properties, property);
    const auto* const whole = held == nullptr
        ? nullptr
        : std::get_if<core::i64>(held);
    if (whole == nullptr || *whole < 0) {
        return false;
    }
    value = doc::NodeId{static_cast<core::u64>(*whole)};
    return true;
}

[[nodiscard]] inline bool read_integer(
    const doc::Node& node, std::string_view property, core::i64 fallback,
    core::i64& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value = fallback;
        return true;
    }
    const auto* const whole = std::get_if<core::i64>(held);
    if (whole == nullptr) {
        return false;
    }
    value = *whole;
    return true;
}

[[nodiscard]] inline bool read_number(
    const doc::Node& node, std::string_view property, float fallback,
    float& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value = fallback;
        return true;
    }
    double wide{};
    if (const auto* const real = std::get_if<double>(held)) {
        wide = *real;
    } else if (const auto* const whole = std::get_if<core::i64>(held)) {
        wide = static_cast<double>(*whole);
    } else {
        return false;
    }
    value = static_cast<float>(wide);
    return std::isfinite(value);
}

[[nodiscard]] inline bool read_boolean(
    const doc::Node& node, std::string_view property, bool fallback,
    bool& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value = fallback;
        return true;
    }
    if (const auto* const boolean = std::get_if<bool>(held)) {
        value = *boolean;
        return true;
    }
    if (const auto* const whole = std::get_if<core::i64>(held)) {
        value = *whole != 0;
        return true;
    }
    if (const auto* const real = std::get_if<double>(held)) {
        if (!std::isfinite(*real)) {
            return false;
        }
        value = *real >= 0.5;
        return true;
    }
    return false;
}

inline void write_reference(doc::PropertyMap& properties,
                            std::string_view name,
                            doc::NodeId value) {
    properties.insert_or_assign(
        std::string{name}, static_cast<core::i64>(value.value()));
}

[[nodiscard]] inline bool valid_utf8(std::string_view value) noexcept {
    std::size_t index{};
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(
            static_cast<unsigned char>(value[index]));
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t continuation_count{};
        std::uint32_t code_point{};
        std::uint32_t minimum{};
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1U;
            code_point = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2U;
            code_point = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (continuation_count > value.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count;
             ++offset) {
            const auto continuation = static_cast<std::uint8_t>(
                static_cast<unsigned char>(value[index + offset]));
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if (code_point < minimum || code_point > 0x10ffffU
            || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

class DocumentIndex final {
public:
    explicit DocumentIndex(const doc::Root& document) {
        for (const doc::NodeChange& change :
             doc::diff(doc::Root{}, document).changes) {
            if (change.after != nullptr) {
                nodes.emplace(change.id, *change.after);
            }
        }
        for (const auto& [id, held] : nodes) {
            if (held.type == schema::stage_type) {
                scenes.insert(id);
            }
        }
    }

    [[nodiscard]] const doc::Node* node(doc::NodeId id) const noexcept {
        const auto found = nodes.find(id);
        return found == nodes.end() ? nullptr : &found->second;
    }

    [[nodiscard]] bool valid_scene(SceneId scene) const noexcept {
        return scene.value() <= max_reference_id && scenes.contains(scene);
    }

    [[nodiscard]] std::size_t mask_count(SceneId scene) const noexcept {
        std::size_t count{};
        for (const auto& [id, held] : nodes) {
            static_cast<void>(id);
            doc::NodeId owner{};
            if (held.type == schema::mask_type
                && read_reference(held, schema::scene_id, owner)
                && owner == scene) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t point_count(SceneId scene) const noexcept {
        std::size_t count{};
        for (const auto& [id, held] : nodes) {
            static_cast<void>(id);
            doc::NodeId owner{};
            if (held.type == schema::mask_point_type
                && read_reference(held, schema::scene_id, owner)
                && owner == scene) {
                ++count;
            }
        }
        return count;
    }

    std::map<doc::NodeId, doc::Node> nodes;
    std::set<SceneId> scenes;
};

[[nodiscard]] inline std::optional<MaskAuthoringError> validate_scene(
    const DocumentIndex& index, SceneId scene) noexcept {
    if (!index.valid_scene(scene)) {
        return error(MaskAuthoringErrorCode::InvalidScene, scene);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<MaskAuthoringError> validate_target(
    const DocumentIndex& index, SceneId scene,
    doc::NodeId target_id) noexcept {
    const doc::Node* const target = index.node(target_id);
    if (target == nullptr) {
        return error(MaskAuthoringErrorCode::TargetMissing, target_id);
    }
    if (target_id.value() > max_reference_id
        || (target->type != schema::object_type
            && target->type != schema::media_type
            && target->type != schema::model_type
            && target->type != schema::group_type)) {
        return error(MaskAuthoringErrorCode::UnsupportedTarget, target_id);
    }
    doc::NodeId owner{};
    if (!read_reference(*target, schema::scene_id, owner)) {
        return error(
            MaskAuthoringErrorCode::MissingTargetSceneOwner, target_id);
    }
    if (owner != scene) {
        return error(MaskAuthoringErrorCode::CrossSceneTarget, target_id,
                     owner);
    }
    return std::nullopt;
}

[[nodiscard]] inline bool valid_combine(MaskCombine combine) noexcept {
    switch (combine) {
    case MaskCombine::Add:
    case MaskCombine::Subtract:
    case MaskCombine::Intersect:
    case MaskCombine::Difference:
        return true;
    }
    return false;
}

[[nodiscard]] inline std::string_view combine_name(
    MaskCombine combine) noexcept {
    switch (combine) {
    case MaskCombine::Add:
        return schema::mask_add;
    case MaskCombine::Subtract:
        return schema::mask_subtract;
    case MaskCombine::Intersect:
        return schema::mask_intersect;
    case MaskCombine::Difference:
        return schema::mask_difference;
    }
    return {};
}

[[nodiscard]] inline bool parse_combine(std::string_view value,
                                        MaskCombine& combine) noexcept {
    if (value == schema::mask_add) {
        combine = MaskCombine::Add;
    } else if (value == schema::mask_subtract) {
        combine = MaskCombine::Subtract;
    } else if (value == schema::mask_intersect) {
        combine = MaskCombine::Intersect;
    } else if (value == schema::mask_difference) {
        combine = MaskCombine::Difference;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] inline std::string_view kind_name(MaskKind kind) noexcept {
    switch (kind) {
    case MaskKind::Rectangle:
        return schema::mask_rectangle_kind;
    case MaskKind::Ellipse:
        return schema::mask_ellipse_kind;
    case MaskKind::Bezier:
        return schema::mask_bezier_kind;
    }
    return {};
}

[[nodiscard]] inline bool parse_kind(std::string_view value,
                                     MaskKind& kind) noexcept {
    if (value == schema::mask_rectangle_kind) {
        kind = MaskKind::Rectangle;
    } else if (value == schema::mask_ellipse_kind) {
        kind = MaskKind::Ellipse;
    } else if (value == schema::mask_bezier_kind) {
        kind = MaskKind::Bezier;
    } else {
        return false;
    }
    return true;
}

struct ParsedMask final {
    doc::Node node;
    SceneId scene{};
    doc::NodeId target{};
    MaskKind kind{MaskKind::Rectangle};
    MaskCombine combine{MaskCombine::Add};
    core::i64 order{};
    bool enabled{true};
};

[[nodiscard]] inline core::Result<ParsedMask, MaskAuthoringError> parse_mask(
    const DocumentIndex& index, SceneId requested,
    doc::NodeId mask_id) {
    using Result = core::Result<ParsedMask, MaskAuthoringError>;
    const doc::Node* const held = index.node(mask_id);
    if (held == nullptr) {
        return Result::failure(
            error(MaskAuthoringErrorCode::MaskNotFound, mask_id));
    }
    if (held->type != schema::mask_type) {
        return Result::failure(
            error(MaskAuthoringErrorCode::NotAMask, mask_id));
    }
    ParsedMask parsed{};
    parsed.node = *held;
    if (!read_reference(*held, schema::scene_id, parsed.scene)
        || !index.valid_scene(parsed.scene)
        || !read_reference(*held, schema::mask_target, parsed.target)) {
        return Result::failure(
            error(MaskAuthoringErrorCode::InvalidMask, mask_id));
    }
    if (parsed.scene != requested) {
        return Result::failure(error(
            MaskAuthoringErrorCode::CrossSceneMask, mask_id, parsed.scene));
    }
    std::string_view kind_value = schema::mask_rectangle_kind;
    if (const auto* const value = find(held->properties, schema::mask_kind)) {
        const auto* const text_value = std::get_if<std::string>(value);
        if (text_value == nullptr) {
            return Result::failure(
                error(MaskAuthoringErrorCode::InvalidMask, mask_id));
        }
        kind_value = *text_value;
    }
    std::string_view combine_value = schema::mask_add;
    if (const auto* const value = find(held->properties,
                                      schema::mask_combine)) {
        const auto* const text_value = std::get_if<std::string>(value);
        if (text_value == nullptr) {
            return Result::failure(
                error(MaskAuthoringErrorCode::InvalidMask, mask_id));
        }
        combine_value = *text_value;
    }
    if (!parse_kind(kind_value, parsed.kind)
        || !parse_combine(combine_value, parsed.combine)
        || !read_integer(*held, schema::mask_order, 0, parsed.order)
        || !read_boolean(*held, schema::mask_enabled, true,
                         parsed.enabled)) {
        return Result::failure(
            error(MaskAuthoringErrorCode::InvalidMask, mask_id));
    }
    return Result::success(std::move(parsed));
}

struct ParsedPoint final {
    doc::Node node;
    SceneId scene{};
    doc::NodeId mask{};
    core::i64 order{};
    MaskPointGeometry geometry{};
};

[[nodiscard]] inline core::Result<ParsedPoint, MaskAuthoringError> parse_point(
    const DocumentIndex& index, SceneId requested, doc::NodeId mask,
    doc::NodeId point_id) {
    using Result = core::Result<ParsedPoint, MaskAuthoringError>;
    const doc::Node* const held = index.node(point_id);
    if (held == nullptr) {
        return Result::failure(
            error(MaskAuthoringErrorCode::PointNotFound, point_id));
    }
    if (held->type != schema::mask_point_type) {
        return Result::failure(
            error(MaskAuthoringErrorCode::NotAMaskPoint, point_id));
    }
    ParsedPoint parsed{};
    parsed.node = *held;
    if (!read_reference(*held, schema::scene_id, parsed.scene)
        || !read_reference(*held, schema::mask_id, parsed.mask)
        || !read_integer(*held, schema::mask_point_order, 0,
                         parsed.order)
        || !read_number(*held, schema::mask_point_x, 0.0F,
                        parsed.geometry.x)
        || !read_number(*held, schema::mask_point_y, 0.0F,
                        parsed.geometry.y)
        || !read_number(*held, schema::mask_point_in_x,
                        parsed.geometry.x, parsed.geometry.in_x)
        || !read_number(*held, schema::mask_point_in_y,
                        parsed.geometry.y, parsed.geometry.in_y)
        || !read_number(*held, schema::mask_point_out_x,
                        parsed.geometry.x, parsed.geometry.out_x)
        || !read_number(*held, schema::mask_point_out_y,
                        parsed.geometry.y, parsed.geometry.out_y)) {
        return Result::failure(
            error(MaskAuthoringErrorCode::InvalidPoint, point_id));
    }
    if (parsed.scene != requested) {
        return Result::failure(error(
            MaskAuthoringErrorCode::CrossScenePoint, point_id, parsed.scene));
    }
    if (parsed.mask != mask) {
        return Result::failure(error(
            MaskAuthoringErrorCode::PointMaskMismatch, point_id,
            parsed.mask));
    }
    return Result::success(std::move(parsed));
}

[[nodiscard]] inline bool canonicalize(MaskPointGeometry& point) noexcept {
    const std::array values{
        point.x, point.y, point.in_x, point.in_y,
        point.out_x, point.out_y};
    if (!std::all_of(values.begin(), values.end(),
                     [](float value) noexcept {
                         return std::isfinite(value);
                     })) {
        return false;
    }
    point.x = std::clamp(point.x, 0.0F, 1.0F);
    point.y = std::clamp(point.y, 0.0F, 1.0F);
    point.in_x = std::clamp(point.in_x, 0.0F, 1.0F);
    point.in_y = std::clamp(point.in_y, 0.0F, 1.0F);
    point.out_x = std::clamp(point.out_x, 0.0F, 1.0F);
    point.out_y = std::clamp(point.out_y, 0.0F, 1.0F);
    return true;
}

[[nodiscard]] inline bool canonicalize(MaskBounds& bounds) noexcept {
    const std::array values{
        bounds.left, bounds.top, bounds.right, bounds.bottom};
    if (!std::all_of(values.begin(), values.end(),
                     [](float value) noexcept {
                         return std::isfinite(value);
                     })) {
        return false;
    }
    bounds.left = std::clamp(bounds.left, 0.0F, 1.0F);
    bounds.top = std::clamp(bounds.top, 0.0F, 1.0F);
    bounds.right = std::clamp(bounds.right, 0.0F, 1.0F);
    bounds.bottom = std::clamp(bounds.bottom, 0.0F, 1.0F);
    return bounds.left < bounds.right && bounds.top < bounds.bottom;
}

[[nodiscard]] inline std::optional<MaskAuthoringError> validate_name(
    std::string_view name, doc::NodeId subject = {}) noexcept {
    if (name.empty() || !valid_utf8(name)) {
        return error(MaskAuthoringErrorCode::InvalidName, subject);
    }
    if (name.size() > max_mask_name_bytes) {
        return error(MaskAuthoringErrorCode::NameTooLong, subject);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<MaskAuthoringError> canonicalize_common(
    float& feather, float& expansion, float& opacity,
    doc::NodeId subject = {}) noexcept {
    if (!std::isfinite(feather) || !std::isfinite(expansion)
        || !std::isfinite(opacity)) {
        return error(MaskAuthoringErrorCode::InvalidValue, subject);
    }
    feather = std::clamp(feather, 0.0F, max_mask_feather_canvas_px);
    expansion = std::clamp(
        expansion, -max_mask_expansion_canvas_px,
        max_mask_expansion_canvas_px);
    opacity = std::clamp(opacity, 0.0F, 1.0F);
    return std::nullopt;
}

class IdAllocator final {
public:
    IdAllocator(const DocumentIndex& index, doc::NodeId first) noexcept
        : index_(index), next_(first.value()) {}

    [[nodiscard]] std::optional<doc::NodeId> take() noexcept {
        while (next_ != 0U && next_ <= max_reference_id) {
            const doc::NodeId candidate{next_};
            if (next_ == max_reference_id) {
                next_ = 0U;
            } else {
                ++next_;
            }
            if (index_.node(candidate) == nullptr
                && !allocated_.contains(candidate)) {
                allocated_.insert(candidate);
                return candidate;
            }
        }
        return std::nullopt;
    }

private:
    const DocumentIndex& index_;
    core::u64 next_{};
    std::set<doc::NodeId> allocated_;
};

[[nodiscard]] inline doc::Node make_point_node(
    doc::NodeId id, SceneId scene, doc::NodeId mask, core::i64 order,
    const MaskPointGeometry& point) {
    doc::Node result{id, std::string{schema::mask_point_type}, {}};
    write_reference(result.properties, schema::scene_id, scene);
    write_reference(result.properties, schema::mask_id, mask);
    result.properties.emplace(std::string{schema::mask_point_order}, order);
    result.properties.emplace(
        std::string{schema::mask_point_x}, static_cast<double>(point.x));
    result.properties.emplace(
        std::string{schema::mask_point_y}, static_cast<double>(point.y));
    result.properties.emplace(
        std::string{schema::mask_point_in_x},
        static_cast<double>(point.in_x));
    result.properties.emplace(
        std::string{schema::mask_point_in_y},
        static_cast<double>(point.in_y));
    result.properties.emplace(
        std::string{schema::mask_point_out_x},
        static_cast<double>(point.out_x));
    result.properties.emplace(
        std::string{schema::mask_point_out_y},
        static_cast<double>(point.out_y));
    return result;
}

inline void write_bounds(doc::PropertyMap& properties,
                         const MaskBounds& bounds) {
    properties.insert_or_assign(
        std::string{schema::mask_left}, static_cast<double>(bounds.left));
    properties.insert_or_assign(
        std::string{schema::mask_top}, static_cast<double>(bounds.top));
    properties.insert_or_assign(
        std::string{schema::mask_right}, static_cast<double>(bounds.right));
    properties.insert_or_assign(
        std::string{schema::mask_bottom}, static_cast<double>(bounds.bottom));
}

inline void write_point_geometry(doc::PropertyMap& properties,
                                 const MaskPointGeometry& point) {
    properties.insert_or_assign(
        std::string{schema::mask_point_x}, static_cast<double>(point.x));
    properties.insert_or_assign(
        std::string{schema::mask_point_y}, static_cast<double>(point.y));
    properties.insert_or_assign(
        std::string{schema::mask_point_in_x},
        static_cast<double>(point.in_x));
    properties.insert_or_assign(
        std::string{schema::mask_point_in_y},
        static_cast<double>(point.in_y));
    properties.insert_or_assign(
        std::string{schema::mask_point_out_x},
        static_cast<double>(point.out_x));
    properties.insert_or_assign(
        std::string{schema::mask_point_out_y},
        static_cast<double>(point.out_y));
}

[[nodiscard]] inline doc::Node make_mask_node(
    doc::NodeId id, const CreateMaskRequest& request,
    MaskKind kind, const MaskBounds& bounds) {
    doc::Node result{id, std::string{schema::mask_type}, {}};
    write_reference(result.properties, schema::scene_id, request.scene);
    write_reference(result.properties, schema::mask_target, request.target);
    result.properties.emplace(
        std::string{schema::mask_name}, request.name);
    result.properties.emplace(
        std::string{schema::mask_kind}, std::string{kind_name(kind)});
    result.properties.emplace(
        std::string{schema::mask_combine},
        std::string{combine_name(request.combine)});
    result.properties.emplace(
        std::string{schema::mask_order}, request.order);
    result.properties.emplace(
        std::string{schema::mask_enabled}, request.enabled);
    result.properties.emplace(
        std::string{schema::mask_feather_px},
        static_cast<double>(request.feather_canvas_px));
    result.properties.emplace(
        std::string{schema::mask_expansion_px},
        static_cast<double>(request.expansion_canvas_px));
    result.properties.emplace(
        std::string{schema::mask_opacity},
        static_cast<double>(request.opacity));
    result.properties.emplace(
        std::string{schema::mask_invert}, request.invert);
    if (kind != MaskKind::Bezier) {
        write_bounds(result.properties, bounds);
        result.properties.emplace(
            std::string{schema::mask_rotation_turns}, 0.0);
    }
    return result;
}

struct OrderedMask final {
    doc::NodeId id{};
    ParsedMask parsed;
};

[[nodiscard]] inline core::Result<std::vector<OrderedMask>,
                                  MaskAuthoringError>
ordered_masks_for_target(const DocumentIndex& index, SceneId scene,
                         doc::NodeId target) {
    using Result = core::Result<std::vector<OrderedMask>,
                                MaskAuthoringError>;
    std::vector<OrderedMask> result;
    for (const auto& [id, held] : index.nodes) {
        if (held.type != schema::mask_type) {
            continue;
        }
        doc::NodeId owner{};
        doc::NodeId held_target{};
        if (!read_reference(held, schema::scene_id, owner)
            || !read_reference(held, schema::mask_target, held_target)
            || owner != scene || held_target != target) {
            continue;
        }
        auto parsed = parse_mask(index, scene, id);
        if (!parsed) {
            return Result::failure(parsed.error());
        }
        result.push_back(OrderedMask{id, std::move(parsed).value()});
    }
    std::sort(result.begin(), result.end(),
              [](const OrderedMask& left,
                 const OrderedMask& right) noexcept {
                  return std::tie(left.parsed.order, left.id)
                      < std::tie(right.parsed.order, right.id);
              });
    return Result::success(std::move(result));
}

[[nodiscard]] inline std::optional<MaskAuthoringError>
validate_first_active(const DocumentIndex& index, SceneId scene,
                      doc::NodeId target) {
    auto ordered = ordered_masks_for_target(index, scene, target);
    if (!ordered) {
        return ordered.error();
    }
    for (const OrderedMask& candidate : ordered.value()) {
        if (!candidate.parsed.enabled) {
            continue;
        }
        if (candidate.parsed.combine != MaskCombine::Add) {
            return error(MaskAuthoringErrorCode::InvalidFirstActiveCombine,
                         candidate.id, target);
        }
        break;
    }
    return std::nullopt;
}

struct OrderedPoint final {
    doc::NodeId id{};
    ParsedPoint parsed;
};

[[nodiscard]] inline core::Result<std::vector<OrderedPoint>,
                                  MaskAuthoringError>
ordered_points_for_mask(const DocumentIndex& index, SceneId scene,
                        doc::NodeId mask) {
    using Result = core::Result<std::vector<OrderedPoint>,
                                MaskAuthoringError>;
    std::vector<OrderedPoint> result;
    for (const auto& [id, held] : index.nodes) {
        if (held.type != schema::mask_point_type) {
            continue;
        }
        doc::NodeId parent{};
        if (!read_reference(held, schema::mask_id, parent)
            || parent != mask) {
            continue;
        }
        auto parsed = parse_point(index, scene, mask, id);
        if (!parsed) {
            return Result::failure(parsed.error());
        }
        result.push_back(OrderedPoint{id, std::move(parsed).value()});
    }
    std::sort(result.begin(), result.end(),
              [](const OrderedPoint& left,
                 const OrderedPoint& right) noexcept {
                  return std::tie(left.parsed.order, left.id)
                      < std::tie(right.parsed.order, right.id);
              });
    for (std::size_t index_value = 1U;
         index_value < result.size(); ++index_value) {
        if (result[index_value - 1U].parsed.order
            == result[index_value].parsed.order) {
            return Result::failure(error(
                MaskAuthoringErrorCode::InvalidPoint,
                result[index_value].id, mask));
        }
    }
    return Result::success(std::move(result));
}

[[nodiscard]] inline MaskAuthoringResult finish(
    const doc::Root& before, std::vector<doc::Node> puts,
    std::vector<doc::NodeId> erases, doc::NodeId mask,
    std::optional<doc::NodeId> point = std::nullopt) {
    std::sort(puts.begin(), puts.end(),
              [](const doc::Node& left, const doc::Node& right) noexcept {
                  return left.id < right.id;
              });
    std::sort(erases.begin(), erases.end());
    erases.erase(std::unique(erases.begin(), erases.end()), erases.end());
    doc::Root after = before;
    for (doc::NodeId id : erases) {
        const auto erased = after.erase(id);
        if (!erased.has_value()) {
            return MaskAuthoringResult::failure(
                error(MaskAuthoringErrorCode::InvalidMask, id, mask));
        }
        after = *erased;
    }
    for (const doc::Node& node : puts) {
        after = after.set(node);
    }
    return MaskAuthoringResult::success(
        MaskAuthoringPlan{std::move(after), std::move(puts),
                          std::move(erases), mask, point});
}

[[nodiscard]] inline std::optional<MaskAuthoringError>
validate_mask_target(const DocumentIndex& index,
                     const ParsedMask& mask) noexcept {
    return validate_target(index, mask.scene, mask.target);
}

[[nodiscard]] inline MaskAuthoringResult allocation_failure(
    doc::NodeId subject = {}) noexcept {
    return MaskAuthoringResult::failure(
        error(MaskAuthoringErrorCode::AllocationFailed, subject));
}

} // namespace mask_authoring_detail

// Creates one shape and, for Bezier, one child node per authored point.
// `first_id` is the first candidate in a collision-skipping ascending scan.
[[nodiscard]] MaskAuthoringResult plan_create_mask(
    const doc::Root& document, CreateMaskRequest request,
    doc::NodeId first_id) {
    namespace detail = mask_authoring_detail;
    try {
        const detail::DocumentIndex index{document};
        if (const auto problem = detail::validate_scene(index, request.scene)) {
            return MaskAuthoringResult::failure(*problem);
        }
        if (const auto problem = detail::validate_target(
                index, request.scene, request.target)) {
            return MaskAuthoringResult::failure(*problem);
        }
        if (index.mask_count(request.scene) >= max_scene_masks) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::MaskCapacity, request.scene));
        }
        if (const auto problem = detail::validate_name(request.name)) {
            return MaskAuthoringResult::failure(*problem);
        }
        if (!detail::valid_combine(request.combine)) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidValue, request.target));
        }
        if (const auto problem = detail::canonicalize_common(
                request.feather_canvas_px, request.expansion_canvas_px,
                request.opacity)) {
            return MaskAuthoringResult::failure(*problem);
        }
        if (first_id.value() == 0U
            || first_id.value() > detail::max_reference_id) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidFirstId, first_id));
        }

        MaskKind kind{MaskKind::Rectangle};
        MaskBounds bounds{};
        std::vector<MaskPointGeometry> points;
        if (const auto* rectangle =
                std::get_if<RectangleMaskShape>(&request.shape)) {
            kind = MaskKind::Rectangle;
            bounds = rectangle->bounds;
            if (!detail::canonicalize(bounds)) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::InvalidShape, request.target));
            }
        } else if (const auto* ellipse =
                       std::get_if<EllipseMaskShape>(&request.shape)) {
            kind = MaskKind::Ellipse;
            bounds = ellipse->bounds;
            if (!detail::canonicalize(bounds)) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::InvalidShape, request.target));
            }
        } else if (const auto* bezier =
                       std::get_if<BezierMaskShape>(&request.shape)) {
            kind = MaskKind::Bezier;
            points = bezier->points;
            if (points.size() < 3U) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::MinimumBezierPoints,
                    request.target));
            }
            if (points.size() > max_mask_points_per_mask
                || points.size()
                    > max_scene_mask_points
                        - std::min<std::size_t>(
                            index.point_count(request.scene),
                            max_scene_mask_points)) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::PointCapacity,
                    request.target));
            }
            for (MaskPointGeometry& point : points) {
                if (!detail::canonicalize(point)) {
                    return MaskAuthoringResult::failure(detail::error(
                        MaskAuthoringErrorCode::InvalidShape,
                        request.target));
                }
            }
        } else {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidShape, request.target));
        }

        detail::IdAllocator allocator{index, first_id};
        const auto mask_id = allocator.take();
        if (!mask_id.has_value()) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::IdExhausted, first_id));
        }
        std::vector<doc::Node> puts;
        puts.reserve(points.size() + 1U);
        puts.push_back(detail::make_mask_node(
            *mask_id, request, kind, bounds));
        for (std::size_t point_index = 0U;
             point_index < points.size(); ++point_index) {
            const auto point_id = allocator.take();
            if (!point_id.has_value()) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::IdExhausted, *mask_id));
            }
            puts.push_back(detail::make_point_node(
                *point_id, request.scene, *mask_id,
                static_cast<core::i64>(point_index),
                points[point_index]));
        }
        auto draft = detail::finish(
            document, puts, {}, *mask_id,
            points.empty()
                ? std::nullopt
                : std::optional<doc::NodeId>{puts[1U].id});
        if (!draft) {
            return draft;
        }
        const detail::DocumentIndex after_index{draft.value().after};
        if (const auto problem = detail::validate_first_active(
                after_index, request.scene, request.target)) {
            return MaskAuthoringResult::failure(*problem);
        }
        return draft;
    } catch (const std::bad_alloc&) {
        return detail::allocation_failure(first_id);
    }
}

[[nodiscard]] MaskAuthoringResult plan_rename_mask(
    const doc::Root& document, SceneId scene, doc::NodeId mask_id,
    std::string name) {
    namespace detail = mask_authoring_detail;
    try {
        const detail::DocumentIndex index{document};
        if (const auto problem = detail::validate_scene(index, scene)) {
            return MaskAuthoringResult::failure(*problem);
        }
        auto parsed = detail::parse_mask(index, scene, mask_id);
        if (!parsed) {
            return MaskAuthoringResult::failure(parsed.error());
        }
        if (const auto problem = detail::validate_mask_target(
                index, parsed.value())) {
            return MaskAuthoringResult::failure(*problem);
        }
        if (const auto problem = detail::validate_name(name, mask_id)) {
            return MaskAuthoringResult::failure(*problem);
        }
        doc::Node updated = parsed.value().node;
        updated.properties.insert_or_assign(
            std::string{schema::mask_name}, std::move(name));
        return detail::finish(document, {std::move(updated)}, {}, mask_id);
    } catch (const std::bad_alloc&) {
        return detail::allocation_failure(mask_id);
    }
}

// Applies only explicitly supplied fields. Finite values are canonicalized to
// the same bounds as SceneModel; equal/reversed shape bounds are rejected.
[[nodiscard]] MaskAuthoringResult plan_edit_mask_properties(
    const doc::Root& document, SceneId scene, doc::NodeId mask_id,
    const MaskPropertyPatch& patch) {
    namespace detail = mask_authoring_detail;
    try {
        const detail::DocumentIndex index{document};
        if (const auto problem = detail::validate_scene(index, scene)) {
            return MaskAuthoringResult::failure(*problem);
        }
        auto parsed = detail::parse_mask(index, scene, mask_id);
        if (!parsed) {
            return MaskAuthoringResult::failure(parsed.error());
        }
        if (const auto problem = detail::validate_mask_target(
                index, parsed.value())) {
            return MaskAuthoringResult::failure(*problem);
        }
        if (patch.combine.has_value()
            && !detail::valid_combine(*patch.combine)) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidValue, mask_id));
        }
        if (patch.bounds.has_value()
            && parsed.value().kind == MaskKind::Bezier) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidShape, mask_id));
        }
        if (patch.rotation_turns.has_value()
            && parsed.value().kind == MaskKind::Bezier) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidShape, mask_id));
        }

        doc::Node updated = parsed.value().node;
        if (patch.enabled.has_value()) {
            updated.properties.insert_or_assign(
                std::string{schema::mask_enabled}, *patch.enabled);
        }
        if (patch.combine.has_value()) {
            updated.properties.insert_or_assign(
                std::string{schema::mask_combine},
                std::string{detail::combine_name(*patch.combine)});
        }
        if (patch.invert.has_value()) {
            updated.properties.insert_or_assign(
                std::string{schema::mask_invert}, *patch.invert);
        }
        if (patch.feather_canvas_px.has_value()) {
            float value = *patch.feather_canvas_px;
            if (!std::isfinite(value)) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::InvalidValue, mask_id));
            }
            value = std::clamp(
                value, 0.0F, max_mask_feather_canvas_px);
            updated.properties.insert_or_assign(
                std::string{schema::mask_feather_px},
                static_cast<double>(value));
        }
        if (patch.expansion_canvas_px.has_value()) {
            float value = *patch.expansion_canvas_px;
            if (!std::isfinite(value)) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::InvalidValue, mask_id));
            }
            value = std::clamp(
                value, -max_mask_expansion_canvas_px,
                max_mask_expansion_canvas_px);
            updated.properties.insert_or_assign(
                std::string{schema::mask_expansion_px},
                static_cast<double>(value));
        }
        if (patch.opacity.has_value()) {
            float value = *patch.opacity;
            if (!std::isfinite(value)) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::InvalidValue, mask_id));
            }
            value = std::clamp(value, 0.0F, 1.0F);
            updated.properties.insert_or_assign(
                std::string{schema::mask_opacity},
                static_cast<double>(value));
        }
        if (patch.bounds.has_value()) {
            MaskBounds bounds = *patch.bounds;
            if (!detail::canonicalize(bounds)) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::InvalidShape, mask_id));
            }
            detail::write_bounds(updated.properties, bounds);
        }
        if (patch.rotation_turns.has_value()) {
            float value = *patch.rotation_turns;
            if (!std::isfinite(value)) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::InvalidValue, mask_id));
            }
            value -= std::floor(value + 0.5F);
            updated.properties.insert_or_assign(
                std::string{schema::mask_rotation_turns},
                static_cast<double>(value));
        }

        auto draft = detail::finish(
            document, {std::move(updated)}, {}, mask_id);
        if (!draft) {
            return draft;
        }
        const detail::DocumentIndex after_index{draft.value().after};
        if (const auto problem = detail::validate_first_active(
                after_index, scene, parsed.value().target)) {
            return MaskAuthoringResult::failure(*problem);
        }
        return draft;
    } catch (const std::bad_alloc&) {
        return detail::allocation_failure(mask_id);
    }
}

[[nodiscard]] MaskAuthoringResult plan_set_mask_enabled(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    bool enabled) {
    MaskPropertyPatch patch{};
    patch.enabled = enabled;
    return plan_edit_mask_properties(document, scene, mask, patch);
}

[[nodiscard]] MaskAuthoringResult plan_set_mask_combine(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    MaskCombine combine) {
    MaskPropertyPatch patch{};
    patch.combine = combine;
    return plan_edit_mask_properties(document, scene, mask, patch);
}

[[nodiscard]] MaskAuthoringResult plan_set_mask_invert(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    bool invert) {
    MaskPropertyPatch patch{};
    patch.invert = invert;
    return plan_edit_mask_properties(document, scene, mask, patch);
}

// Moves a mask within its target-local `(order,node id)` sequence and rewrites
// that sequence to dense orders [0,n). Disabled masks retain their positions.
[[nodiscard]] MaskAuthoringResult plan_reorder_mask(
    const doc::Root& document, SceneId scene, doc::NodeId mask_id,
    std::size_t destination_index) {
    namespace detail = mask_authoring_detail;
    try {
        const detail::DocumentIndex index{document};
        if (const auto problem = detail::validate_scene(index, scene)) {
            return MaskAuthoringResult::failure(*problem);
        }
        auto parsed = detail::parse_mask(index, scene, mask_id);
        if (!parsed) {
            return MaskAuthoringResult::failure(parsed.error());
        }
        if (const auto problem = detail::validate_mask_target(
                index, parsed.value())) {
            return MaskAuthoringResult::failure(*problem);
        }
        auto ordered = detail::ordered_masks_for_target(
            index, scene, parsed.value().target);
        if (!ordered) {
            return MaskAuthoringResult::failure(ordered.error());
        }
        if (destination_index >= ordered.value().size()) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidMaskIndex, mask_id));
        }
        const auto current = std::find_if(
            ordered.value().begin(), ordered.value().end(),
            [mask_id](const detail::OrderedMask& candidate) noexcept {
                return candidate.id == mask_id;
            });
        if (current == ordered.value().end()) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::MaskNotFound, mask_id));
        }
        detail::OrderedMask moving = *current;
        ordered.value().erase(current);
        ordered.value().insert(
            ordered.value().begin()
                + static_cast<std::ptrdiff_t>(destination_index),
            std::move(moving));

        std::vector<doc::Node> puts;
        puts.reserve(ordered.value().size());
        for (std::size_t position = 0U;
             position < ordered.value().size(); ++position) {
            doc::Node updated = ordered.value()[position].parsed.node;
            const core::i64 new_order = static_cast<core::i64>(position);
            updated.properties.insert_or_assign(
                std::string{schema::mask_order}, new_order);
            if (updated != ordered.value()[position].parsed.node) {
                puts.push_back(std::move(updated));
            }
        }
        auto draft = detail::finish(document, std::move(puts), {}, mask_id);
        if (!draft) {
            return draft;
        }
        const detail::DocumentIndex after_index{draft.value().after};
        if (const auto problem = detail::validate_first_active(
                after_index, scene, parsed.value().target)) {
            return MaskAuthoringResult::failure(*problem);
        }
        return draft;
    } catch (const std::bad_alloc&) {
        return detail::allocation_failure(mask_id);
    }
}

// Deletes a mask and every typed child that explicitly references it. The
// target's remaining active chain must still begin with Add.
[[nodiscard]] MaskAuthoringResult plan_delete_mask(
    const doc::Root& document, SceneId scene, doc::NodeId mask_id) {
    namespace detail = mask_authoring_detail;
    try {
        const detail::DocumentIndex index{document};
        if (const auto problem = detail::validate_scene(index, scene)) {
            return MaskAuthoringResult::failure(*problem);
        }
        auto parsed = detail::parse_mask(index, scene, mask_id);
        if (!parsed) {
            return MaskAuthoringResult::failure(parsed.error());
        }
        if (const auto problem = detail::validate_mask_target(
                index, parsed.value())) {
            return MaskAuthoringResult::failure(*problem);
        }
        std::vector<doc::NodeId> erases{mask_id};
        for (const auto& [id, held] : index.nodes) {
            doc::NodeId parent{};
            if (held.type == schema::mask_point_type
                && detail::read_reference(
                    held, schema::mask_id, parent)
                && parent == mask_id) {
                erases.push_back(id);
            }
        }
        auto draft = detail::finish(
            document, {}, std::move(erases), mask_id);
        if (!draft) {
            return draft;
        }
        const detail::DocumentIndex after_index{draft.value().after};
        if (const auto problem = detail::validate_first_active(
                after_index, scene, parsed.value().target)) {
            return MaskAuthoringResult::failure(*problem);
        }
        return draft;
    } catch (const std::bad_alloc&) {
        return detail::allocation_failure(mask_id);
    }
}

// Inserts a point at a final dense child index. Existing child node ids remain
// stable; only their explicit order properties may change.
[[nodiscard]] MaskAuthoringResult plan_insert_mask_point(
    const doc::Root& document, SceneId scene, doc::NodeId mask_id,
    std::size_t index_value, MaskPointGeometry geometry,
    doc::NodeId first_id) {
    namespace detail = mask_authoring_detail;
    try {
        const detail::DocumentIndex index{document};
        if (const auto problem = detail::validate_scene(index, scene)) {
            return MaskAuthoringResult::failure(*problem);
        }
        auto mask = detail::parse_mask(index, scene, mask_id);
        if (!mask) {
            return MaskAuthoringResult::failure(mask.error());
        }
        if (const auto problem = detail::validate_mask_target(
                index, mask.value())) {
            return MaskAuthoringResult::failure(*problem);
        }
        if (mask.value().kind != MaskKind::Bezier) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::NonBezierMask, mask_id));
        }
        auto points = detail::ordered_points_for_mask(index, scene, mask_id);
        if (!points) {
            return MaskAuthoringResult::failure(points.error());
        }
        if (index_value > points.value().size()) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidPointIndex, mask_id));
        }
        if (points.value().size() >= max_mask_points_per_mask
            || index.point_count(scene) >= max_scene_mask_points) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::PointCapacity, mask_id));
        }
        if (!detail::canonicalize(geometry)) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidValue, mask_id));
        }
        if (first_id.value() == 0U
            || first_id.value() > detail::max_reference_id) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidFirstId, first_id));
        }
        detail::IdAllocator allocator{index, first_id};
        const auto point_id = allocator.take();
        if (!point_id.has_value()) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::IdExhausted, first_id));
        }

        detail::OrderedPoint inserted{};
        inserted.id = *point_id;
        inserted.parsed.node = detail::make_point_node(
            *point_id, scene, mask_id,
            static_cast<core::i64>(index_value), geometry);
        inserted.parsed.scene = scene;
        inserted.parsed.mask = mask_id;
        inserted.parsed.order = static_cast<core::i64>(index_value);
        inserted.parsed.geometry = geometry;
        points.value().insert(
            points.value().begin()
                + static_cast<std::ptrdiff_t>(index_value),
            std::move(inserted));

        std::vector<doc::Node> puts;
        puts.reserve(points.value().size());
        for (std::size_t position = 0U;
             position < points.value().size(); ++position) {
            doc::Node updated = points.value()[position].parsed.node;
            updated.properties.insert_or_assign(
                std::string{schema::mask_point_order},
                static_cast<core::i64>(position));
            if (points.value()[position].id == *point_id
                || updated != points.value()[position].parsed.node) {
                puts.push_back(std::move(updated));
            }
        }
        return detail::finish(
            document, std::move(puts), {}, mask_id, *point_id);
    } catch (const std::bad_alloc&) {
        return detail::allocation_failure(mask_id);
    }
}

[[nodiscard]] MaskAuthoringResult plan_edit_mask_point(
    const doc::Root& document, SceneId scene, doc::NodeId mask_id,
    doc::NodeId point_id, const MaskPointPatch& patch) {
    namespace detail = mask_authoring_detail;
    try {
        const detail::DocumentIndex index{document};
        if (const auto problem = detail::validate_scene(index, scene)) {
            return MaskAuthoringResult::failure(*problem);
        }
        auto mask = detail::parse_mask(index, scene, mask_id);
        if (!mask) {
            return MaskAuthoringResult::failure(mask.error());
        }
        if (const auto problem = detail::validate_mask_target(
                index, mask.value())) {
            return MaskAuthoringResult::failure(*problem);
        }
        if (mask.value().kind != MaskKind::Bezier) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::NonBezierMask, mask_id));
        }
        auto point = detail::parse_point(
            index, scene, mask_id, point_id);
        if (!point) {
            return MaskAuthoringResult::failure(point.error());
        }
        auto points = detail::ordered_points_for_mask(index, scene, mask_id);
        if (!points) {
            return MaskAuthoringResult::failure(points.error());
        }
        if (std::none_of(
                points.value().begin(), points.value().end(),
                [point_id](const detail::OrderedPoint& candidate) noexcept {
                    return candidate.id == point_id;
                })) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::PointMaskMismatch,
                point_id, mask_id));
        }
        MaskPointGeometry geometry = point.value().geometry;
        if (patch.x.has_value()) {
            geometry.x = *patch.x;
        }
        if (patch.y.has_value()) {
            geometry.y = *patch.y;
        }
        if (patch.in_x.has_value()) {
            geometry.in_x = *patch.in_x;
        }
        if (patch.in_y.has_value()) {
            geometry.in_y = *patch.in_y;
        }
        if (patch.out_x.has_value()) {
            geometry.out_x = *patch.out_x;
        }
        if (patch.out_y.has_value()) {
            geometry.out_y = *patch.out_y;
        }
        if (!detail::canonicalize(geometry)) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::InvalidValue, point_id));
        }
        doc::Node updated = point.value().node;
        detail::write_point_geometry(updated.properties, geometry);
        return detail::finish(
            document, {std::move(updated)}, {}, mask_id, point_id);
    } catch (const std::bad_alloc&) {
        return detail::allocation_failure(point_id);
    }
}

[[nodiscard]] MaskAuthoringResult plan_delete_mask_point(
    const doc::Root& document, SceneId scene, doc::NodeId mask_id,
    doc::NodeId point_id) {
    namespace detail = mask_authoring_detail;
    try {
        const detail::DocumentIndex index{document};
        if (const auto problem = detail::validate_scene(index, scene)) {
            return MaskAuthoringResult::failure(*problem);
        }
        auto mask = detail::parse_mask(index, scene, mask_id);
        if (!mask) {
            return MaskAuthoringResult::failure(mask.error());
        }
        if (const auto problem = detail::validate_mask_target(
                index, mask.value())) {
            return MaskAuthoringResult::failure(*problem);
        }
        if (mask.value().kind != MaskKind::Bezier) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::NonBezierMask, mask_id));
        }
        auto points = detail::ordered_points_for_mask(index, scene, mask_id);
        if (!points) {
            return MaskAuthoringResult::failure(points.error());
        }
        const auto removing = std::find_if(
            points.value().begin(), points.value().end(),
            [point_id](const detail::OrderedPoint& candidate) noexcept {
                return candidate.id == point_id;
            });
        if (removing == points.value().end()) {
            const doc::Node* const held = index.node(point_id);
            if (held == nullptr) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::PointNotFound, point_id));
            }
            if (held->type != schema::mask_point_type) {
                return MaskAuthoringResult::failure(detail::error(
                    MaskAuthoringErrorCode::NotAMaskPoint, point_id));
            }
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::PointMaskMismatch,
                point_id, mask_id));
        }
        if (points.value().size() <= 3U) {
            return MaskAuthoringResult::failure(detail::error(
                MaskAuthoringErrorCode::MinimumBezierPoints,
                point_id, mask_id));
        }
        points.value().erase(removing);
        std::vector<doc::Node> puts;
        puts.reserve(points.value().size());
        for (std::size_t position = 0U;
             position < points.value().size(); ++position) {
            doc::Node updated = points.value()[position].parsed.node;
            updated.properties.insert_or_assign(
                std::string{schema::mask_point_order},
                static_cast<core::i64>(position));
            if (updated != points.value()[position].parsed.node) {
                puts.push_back(std::move(updated));
            }
        }
        return detail::finish(
            document, std::move(puts), {point_id}, mask_id, point_id);
    } catch (const std::bad_alloc&) {
        return detail::allocation_failure(point_id);
    }
}


} // namespace mediaserver::scene
