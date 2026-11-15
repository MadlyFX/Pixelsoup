#include <mediaserver/scene/scene_model.hpp>

#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/instance_batch.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace mediaserver::scene {

doc::Root without_asset_commits(const doc::Root& project) {
    auto result = project;
    for (const auto& change : doc::diff(doc::Root{}, project).changes) {
        const auto* source = project.get(change.id);
        if (!source || source->type != schema::stage_type) continue;
        auto node = *source;
        std::size_t removed = 0;
        for (const auto key : {publication::candidate, schema::asset_commit_digest,
                               schema::asset_commit_control,
                               schema::asset_commit_show})
            removed += node.properties.erase(std::string{key});
        if (removed) result = result.set(std::move(node));
    }
    return result;
}

doc::Root without_publication_locks(const doc::Root& project) {
    std::set<core::u64> published;
    for (const auto& change : doc::diff(doc::Root{}, project).changes)
        if (change.after->type == schema::stage_type && !publication::text(change.after.get(), publication::revision).empty())
            published.insert(change.id.value());
    if (published.empty()) return project;
    auto result = project;
    for (const auto& change : doc::diff(doc::Root{}, project).changes) {
        const auto& node = *change.after;
        if (!published.contains(change.id.value()) && !published.contains(publication::integer(&node, schema::scene_id))) continue;
        if (!node.properties.contains(schema::locked)) continue;
        auto unlocked = node; unlocked.properties.erase(std::string{schema::locked});
        result = result.set(std::move(unlocked));
    }
    return result;
}

doc::PropertyMap baked_lighting_properties(const BakedLighting& value) {
    const auto baked = clamped(value);
    doc::PropertyMap result;
    result.emplace(std::string{schema::baked_enabled}, doc::PropertyValue{baked.enabled});
    result.emplace(std::string{schema::baked_min_x}, doc::PropertyValue{static_cast<double>(baked.minimum.x)});
    result.emplace(std::string{schema::baked_min_y}, doc::PropertyValue{static_cast<double>(baked.minimum.y)});
    result.emplace(std::string{schema::baked_min_z}, doc::PropertyValue{static_cast<double>(baked.minimum.z)});
    result.emplace(std::string{schema::baked_max_x}, doc::PropertyValue{static_cast<double>(baked.maximum.x)});
    result.emplace(std::string{schema::baked_max_y}, doc::PropertyValue{static_cast<double>(baked.maximum.y)});
    result.emplace(std::string{schema::baked_max_z}, doc::PropertyValue{static_cast<double>(baked.maximum.z)});
    result.emplace(std::string{schema::baked_count_x}, doc::PropertyValue{static_cast<core::i64>(baked.count_x)});
    result.emplace(std::string{schema::baked_count_y}, doc::PropertyValue{static_cast<core::i64>(baked.count_y)});
    result.emplace(std::string{schema::baked_count_z}, doc::PropertyValue{static_cast<core::i64>(baked.count_z)});
    result.emplace(std::string{schema::baked_samples}, doc::PropertyValue{static_cast<core::i64>(baked.samples)});
    result.emplace(std::string{schema::baked_bounces}, doc::PropertyValue{static_cast<core::i64>(baked.bounces)});
    result.emplace(std::string{schema::baked_strength}, doc::PropertyValue{static_cast<double>(baked.strength)});
    result.emplace(std::string{schema::baked_source}, doc::PropertyValue{baked.source});
    result.emplace(std::string{schema::baked_content}, doc::PropertyValue{baked.content});
    result.emplace(std::string{schema::baked_input_revision}, doc::PropertyValue{baked.input_revision});
    for (std::size_t i = 0; i < baked.reflections.size(); ++i) {
        const auto& p = baked.reflections[i];
        const auto prefix = "reflection_" + std::to_string(i) + "_";
        result.emplace(prefix + "node", doc::PropertyValue{static_cast<core::i64>(p.source.value())});
        result.emplace(prefix + "enabled", doc::PropertyValue{p.enabled});
        result.emplace(prefix + "sphere", doc::PropertyValue{p.sphere});
        const auto number = [&](const char* key, float v) { result.emplace(prefix + key, doc::PropertyValue{static_cast<double>(v)}); };
        number("x", p.position.x); number("y", p.position.y); number("z", p.position.z);
        number("extent_x", p.extent.x); number("extent_y", p.extent.y); number("extent_z", p.extent.z);
        number("blend", p.blend); number("intensity", p.intensity);
    }
    return result;
}

doc::PropertyMap planar_reflection_properties(const PlanarReflection& value) {
    const auto p = clamped(value);
    doc::PropertyMap result;
    result.emplace("planar_enabled", doc::PropertyValue{p.enabled});
    result.emplace("planar_receiver", doc::PropertyValue{static_cast<core::i64>(p.receiver.value())});
    const auto number = [&](const char* key, float v) { result.emplace(key, doc::PropertyValue{static_cast<double>(v)}); };
    number("planar_x", p.point.x); number("planar_y", p.point.y); number("planar_z", p.point.z);
    number("planar_nx", p.normal.x); number("planar_ny", p.normal.y); number("planar_nz", p.normal.z);
    number("planar_strength", p.strength);
    return result;
}

namespace {

// The vocabulary names the slots and the evaluated scene counts them.
static_assert(schema::vehicle_slot_count == vehicle_slot_count);

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties,
    std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

[[nodiscard]] core::u64 ordered_frame(core::i64 frame) noexcept {
    return static_cast<core::u64>(frame) ^ (core::u64{1U} << 63U);
}

[[nodiscard]] core::u64 frame_distance(core::i64 left,
                                       core::i64 right) noexcept {
    return ordered_frame(right) - ordered_frame(left);
}

[[nodiscard]] bool same_direction(double left, double right) noexcept {
    return (left > 0.0 && right > 0.0)
        || (left < 0.0 && right < 0.0);
}

[[nodiscard]] double cubic_bezier(double first, double first_handle,
                                  double second_handle, double second,
                                  double t) noexcept {
    const double inverse = 1.0 - t;
    const double inverse_squared = inverse * inverse;
    const double t_squared = t * t;
    return inverse_squared * inverse * first
        + 3.0 * inverse_squared * t * first_handle
        + 3.0 * inverse * t_squared * second_handle
        + t_squared * t * second;
}

// A property reader that fails loudly rather than quietly.
//
// A property that is absent takes its default: documents should not have to
// spell out every field. A property that is *present with the wrong type*
// rejects the whole node instead, because that is a typo or a schema mismatch, and
// silently substituting a default there would leave an operator staring at an
// object in the wrong place with no indication why.
class Reader final {
public:
    explicit Reader(const doc::PropertyMap& properties) noexcept
        : properties_(&properties) {}

    [[nodiscard]] float number(std::string_view name, float fallback) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        double wide{};
        if (const auto* const real = std::get_if<double>(value)) {
            wide = *real;
        } else if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            // An operator who types 3 for a scale should get 3.0, not a
            // rejected node.
            wide = static_cast<double>(*whole);
        } else {
            ok_ = false;
            return fallback;
        }
        const auto narrow = static_cast<float>(wide);
        if (!std::isfinite(narrow)) {
            ok_ = false;
            return fallback;
        }
        return narrow;
    }

    // Effect controls whose contract defines malformed numeric values as
    // identity use this path. A wrong property *type* still rejects the node;
    // a numeric value outside the processor's domain is data, not a schema
    // mismatch, and deterministically becomes the supplied identity.
    [[nodiscard]] float bounded_number_or_identity(
        std::string_view name, float identity, float minimum,
        float maximum) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return identity;
        }
        double wide{};
        if (const auto* const real = std::get_if<double>(value)) {
            wide = *real;
        } else if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            wide = static_cast<double>(*whole);
        } else {
            ok_ = false;
            return identity;
        }
        const float narrow = static_cast<float>(wide);
        return std::isfinite(narrow) && narrow >= minimum &&
                narrow <= maximum
            ? narrow
            : identity;
    }

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

    [[nodiscard]] std::string_view text(
        std::string_view name,
        std::string_view fallback) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* const string = std::get_if<std::string>(value)) {
            return *string;
        }
        ok_ = false;
        return fallback;
    }

    [[nodiscard]] bool boolean(std::string_view name, bool fallback) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* const boolean_value = std::get_if<bool>(value)) {
            return *boolean_value;
        }
        // Early editor builds authored enable flags as 0/1 numbers. Accepting
        // those keeps such documents readable while new effect nodes use bool.
        if (const auto* const real = std::get_if<double>(value)) {
            if (!std::isfinite(*real)) {
                ok_ = false;
                return fallback;
            }
            return *real >= 0.5;
        }
        if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            return *whole != 0;
        }
        ok_ = false;
        return fallback;
    }

    [[nodiscard]] Vec3 vector(
        std::string_view x,
        std::string_view y,
        std::string_view z,
        Vec3 fallback) noexcept {
        return Vec3{
            number(x, fallback.x), number(y, fallback.y), number(z, fallback.z)};
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }

private:
    const doc::PropertyMap* properties_;
    bool ok_{true};
};

[[nodiscard]] std::optional<LevelsParameters>
read_levels(Reader& reader) noexcept {
    LevelsParameters value{};
    const auto read_channel = [&reader](LevelsChannelParameters& channel,
                                        std::string_view input_black,
                                        std::string_view input_white,
                                        std::string_view gamma,
                                        std::string_view output_black,
                                        std::string_view output_white) noexcept {
        channel.input_black = reader.number(input_black, 0.0F);
        channel.input_white = reader.number(input_white, 1.0F);
        channel.gamma = reader.number(gamma, 1.0F);
        channel.output_black = reader.number(output_black, 0.0F);
        channel.output_white = reader.number(output_white, 1.0F);
    };
    read_channel(value.master, schema::levels_master_input_black,
                 schema::levels_master_input_white,
                 schema::levels_master_gamma,
                 schema::levels_master_output_black,
                 schema::levels_master_output_white);
    read_channel(value.red, schema::levels_red_input_black,
                 schema::levels_red_input_white, schema::levels_red_gamma,
                 schema::levels_red_output_black,
                 schema::levels_red_output_white);
    read_channel(value.green, schema::levels_green_input_black,
                 schema::levels_green_input_white, schema::levels_green_gamma,
                 schema::levels_green_output_black,
                 schema::levels_green_output_white);
    read_channel(value.blue, schema::levels_blue_input_black,
                 schema::levels_blue_input_white, schema::levels_blue_gamma,
                 schema::levels_blue_output_black,
                 schema::levels_blue_output_white);
    return reader.ok() ? normalized_levels(value) : std::nullopt;
}

[[nodiscard]] std::optional<LiftGammaGainParameters>
read_lift_gamma_gain(Reader& reader) noexcept {
    LiftGammaGainParameters value{};
    value.lift = reader.vector(
        schema::lift_r, schema::lift_g, schema::lift_b, value.lift);
    value.gamma = reader.vector(
        schema::gamma_r, schema::gamma_g, schema::gamma_b, value.gamma);
    value.gain = reader.vector(
        schema::gain_r, schema::gain_g, schema::gain_b, value.gain);
    return reader.ok() ? normalized_lift_gamma_gain(value) : std::nullopt;
}

[[nodiscard]] std::optional<ContrastPivotParameters>
read_contrast_pivot(Reader& reader) noexcept {
    ContrastPivotParameters value{};
    value.contrast = reader.number(schema::contrast, value.contrast);
    value.pivot = reader.number(schema::pivot, value.pivot);
    return reader.ok() ? normalized_contrast_pivot(value) : std::nullopt;
}

[[nodiscard]] std::optional<ColorMatrixParameters>
read_color_matrix(Reader& reader) noexcept {
    ColorMatrixParameters value{};
    const std::array<std::string_view, 9U> properties{
        schema::m00, schema::m01, schema::m02,
        schema::m10, schema::m11, schema::m12,
        schema::m20, schema::m21, schema::m22};
    for (std::size_t index = 0U; index < properties.size(); ++index) {
        value.matrix[index] =
            reader.number(properties[index], value.matrix[index]);
    }
    value.offset = reader.vector(
        schema::offset_r, schema::offset_g, schema::offset_b, value.offset);
    return reader.ok() ? normalized_color_matrix(value) : std::nullopt;
}

[[nodiscard]] std::optional<WhiteBalanceParameters>
read_white_balance(Reader& reader) noexcept {
    WhiteBalanceParameters value{};
    value.temperature_kelvin = reader.number(
        schema::temperature_kelvin, value.temperature_kelvin);
    value.tint = reader.number(schema::tint, value.tint);
    return reader.ok() ? normalized_white_balance(value) : std::nullopt;
}

[[nodiscard]] std::optional<CurveChannelParameters> read_curve_channel(
    Reader& reader, const doc::PropertyMap& properties,
    std::string_view prefix, bool required) {
    CurveChannelParameters channel{};
    channel.points.clear();
    bool gap{};
    for (core::u32 index = 0U; index <= max_curve_control_points; ++index) {
        const std::string base = std::string{prefix} + std::to_string(index);
        const std::string x_name = base + std::string{schema::curve_x_suffix};
        const std::string y_name = base + std::string{schema::curve_y_suffix};
        const bool has_x = find(properties, x_name) != nullptr;
        const bool has_y = find(properties, y_name) != nullptr;
        if (!has_x && !has_y) {
            gap = true;
            continue;
        }
        if (!has_x || !has_y || gap || index == max_curve_control_points) {
            reader.fail();
            return std::nullopt;
        }
        channel.points.push_back(CurveControlPoint{
            reader.number(x_name, 0.0F), reader.number(y_name, 0.0F)});
    }
    if (channel.points.empty()) {
        return required
            ? std::optional<CurveChannelParameters>{CurveChannelParameters{}}
            : std::nullopt;
    }
    return channel;
}

[[nodiscard]] std::optional<CurvesParameters> read_curves(
    Reader& reader, const doc::PropertyMap& properties) {
    CurvesParameters value{};
    const auto master = read_curve_channel(
        reader, properties, schema::curve_master_prefix, true);
    if (!master.has_value()) {
        reader.fail();
        return std::nullopt;
    }
    value.master = *master;
    value.red = read_curve_channel(
        reader, properties, schema::curve_red_prefix, false);
    value.green = read_curve_channel(
        reader, properties, schema::curve_green_prefix, false);
    value.blue = read_curve_channel(
        reader, properties, schema::curve_blue_prefix, false);
    return reader.ok() ? normalized_curves(value) : std::nullopt;
}

// An integer property that has to fit an unsigned 32-bit field. Rejects rather
// than clamps: a negative width is not a small width.
[[nodiscard]] bool as_u32(std::int64_t value, core::u32& into) noexcept {
    if (value < 0 || value > static_cast<std::int64_t>(UINT32_MAX)) {
        return false;
    }
    into = static_cast<core::u32>(value);
    return true;
}

[[nodiscard]] bool as_i32(std::int64_t value, core::i32& into) noexcept {
    if (value < static_cast<std::int64_t>(
                    std::numeric_limits<core::i32>::min()) ||
        value > static_cast<std::int64_t>(
                    std::numeric_limits<core::i32>::max())) {
        return false;
    }
    into = static_cast<core::i32>(value);
    return true;
}

[[nodiscard]] std::optional<FilmGrainParameters>
read_film_grain(Reader& reader) noexcept {
    FilmGrainParameters value{};
    value.amount = reader.number(schema::amount, value.amount);
    value.pixel_scale =
        reader.number(schema::pixel_scale, value.pixel_scale);
    value.chroma_ratio =
        reader.number(schema::chroma_ratio, value.chroma_ratio);
    if (!as_u32(reader.integer(schema::seed, 0), value.seed)) {
        reader.fail();
    }
    const std::string_view mode =
        reader.text(schema::mode, schema::monochrome_mode);
    if (mode == schema::monochrome_mode) {
        value.mode = FilmGrainMode::Monochrome;
    } else if (mode == schema::rgb_mode) {
        value.mode = FilmGrainMode::RGB;
    } else {
        reader.fail();
    }
    return reader.ok() ? normalized_film_grain(value) : std::nullopt;
}

[[nodiscard]] std::optional<ChromaKeyParameters>
read_chroma_key(Reader& reader) noexcept {
    ChromaKeyParameters value{};
    value.mix = reader.number(schema::chroma_key_mix, value.mix);
    // Mix zero is semantically and operationally inert. Canonicalize it
    // before reading the irrelevant payload so stale/malformed controls cannot
    // turn an authored bypass into a rejected scene.
    if (!reader.ok() || value.mix == 0.0F) {
        return reader.ok() ? normalized_chroma_key(value) : std::nullopt;
    }
    value.key_color = reader.vector(
        schema::chroma_key_color_r, schema::chroma_key_color_g,
        schema::chroma_key_color_b, value.key_color);
    value.tolerance =
        reader.number(schema::chroma_key_tolerance, value.tolerance);
    value.softness =
        reader.number(schema::chroma_key_softness, value.softness);
    if (!as_i32(reader.integer(schema::chroma_key_morphology_px, 0),
                value.morphology_px)) {
        reader.fail();
    }
    value.spill_suppression = reader.number(
        schema::chroma_key_spill_suppression, value.spill_suppression);
    return reader.ok() ? normalized_chroma_key(value) : std::nullopt;
}

[[nodiscard]] std::optional<LumaKeyParameters>
read_luma_key(Reader& reader) noexcept {
    LumaKeyParameters value{};
    value.mix = reader.number(schema::luma_key_mix, value.mix);
    if (!reader.ok() || value.mix == 0.0F) {
        return reader.ok() ? normalized_luma_key(value) : std::nullopt;
    }
    value.low_threshold =
        reader.number(schema::luma_key_low_threshold, value.low_threshold);
    value.high_threshold = reader.number(
        schema::luma_key_high_threshold, value.high_threshold);
    value.low_softness =
        reader.number(schema::luma_key_low_softness, value.low_softness);
    value.high_softness =
        reader.number(schema::luma_key_high_softness, value.high_softness);
    value.invert = reader.boolean(schema::luma_key_invert, value.invert);
    return reader.ok() ? normalized_luma_key(value) : std::nullopt;
}

// (left + right) modulo modulus without overflowing. Callers keep both
// operands below the modulus; the branch form is also safe when the modulus is
// close to UINT64_MAX.
[[nodiscard]] core::u64 add_mod(core::u64 left, core::u64 right,
                                core::u64 modulus) noexcept {
    return left >= modulus - right
        ? left - (modulus - right)
        : left + right;
}

// Exact floor(show_frame * source_rate / show_rate), reduced modulo the clip
// length. A direct triple product can exceed 64 bits for long-running shows,
// so this consumes the show-frame bits while carrying the division remainder.
// The recurrence is integer-only and therefore deterministic on every node.
[[nodiscard]] core::u32 media_frame_within(
    SceneTime time, MediaFrameRate source_rate,
    core::u32 sequence_count) noexcept {
    if (sequence_count == 0U || source_rate.numerator == 0U ||
        source_rate.denominator == 0U || time.rate_numerator == 0U ||
        time.rate_denominator == 0U) {
        return sequence_count == 0U
            ? 0U
            : static_cast<core::u32>(time.frame % sequence_count);
    }

    const core::u64 numerator =
        static_cast<core::u64>(source_rate.numerator) *
        static_cast<core::u64>(time.rate_denominator);
    const core::u64 denominator =
        static_cast<core::u64>(source_rate.denominator) *
        static_cast<core::u64>(time.rate_numerator);
    // Every show a person will run keeps the product inside 64 bits, where
    // the same exact quotient is one division. The recurrence below is for
    // the frames that would overflow it.
    if (time.frame <= std::numeric_limits<core::u64>::max() / numerator) {
        return static_cast<core::u32>(
            (time.frame * numerator / denominator) % sequence_count);
    }
    const core::u64 quotient_step = numerator / denominator;
    const core::u64 remainder_step = numerator % denominator;
    const core::u64 modulus = sequence_count;

    core::u64 quotient_mod{};
    core::u64 remainder{};
    for (int bit = 63; bit >= 0; --bit) {
        quotient_mod = add_mod(quotient_mod, quotient_mod, modulus);

        core::u64 carry{};
        const bool doubled_wraps = remainder >= denominator - remainder;
        remainder = doubled_wraps
            ? remainder - (denominator - remainder)
            : remainder + remainder;
        carry += doubled_wraps ? 1U : 0U;

        if (((time.frame >> bit) & 1U) != 0U) {
            quotient_mod = add_mod(
                quotient_mod, quotient_step % modulus, modulus);
            const bool added_wraps =
                remainder >= denominator - remainder_step;
            remainder = added_wraps
                ? remainder - (denominator - remainder_step)
                : remainder + remainder_step;
            carry += added_wraps ? 1U : 0U;
        }
        quotient_mod = add_mod(quotient_mod, carry % modulus, modulus);
    }
    return static_cast<core::u32>(quotient_mod);
}

// Whether the show's own repeat wraps within `lead` frames of now. That wrap is
// a discontinuity no clip announces: a source is rarely the length of the show,
// so a clip still on air on the far side of it lands on an unrelated part of
// its own file, with nothing decoded and no cut to have warned anyone.
[[nodiscard]] bool show_wraps_within(
    const ShowLoop& loop, core::i64 now, core::i64 lead) noexcept {
    if (!loop.enabled || loop.last < loop.first || lead <= 0) {
        return false;
    }
    constexpr auto ceiling =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    const auto first = static_cast<core::i64>(std::min(loop.first, ceiling));
    const auto last = static_cast<core::i64>(std::min(loop.last, ceiling));
    if (now < first || now > last) {
        return false;
    }
    return last - now < lead;
}

// Which source frame a clip shows at a show frame. The drawing path and the
// warm-up path both go through here, because a frame decoded ahead of a cut is
// worth nothing unless it is the exact frame the cut then asks for.
[[nodiscard]] core::u32 clip_frame_at(
    SceneTime time, core::u64 at_show_frame, const ClipTiming& timing,
    const std::optional<MediaFrameRate>& source_rate,
    core::u32 sequence_start, core::u32 sequence_count) noexcept {
    if (sequence_count == 0U) {
        return sequence_start;
    }
    // A placed clip counts from its own first frame, so trimming or moving it
    // on the timeline changes which source frame it shows without changing the
    // source. An unplaced one counts from show frame zero exactly as it always
    // did - at full 64-bit width, because a show ordinal is not bounded by the
    // signed arithmetic that placement needs. Asking before a clip starts
    // answers with its first frame, which is what a warm-up wants and what a
    // draw would have asked for anyway.
    SceneTime clip_time = time;
    if (timing.placed()) {
        // A show never reaches 2^63 frames; the saturation exists so the
        // signed placement subtraction has no undefined edge at all.
        const auto placed_frame = static_cast<core::i64>(std::min(
            at_show_frame,
            static_cast<core::u64>(std::numeric_limits<core::i64>::max())));
        clip_time.frame = placed_frame > timing.start
            ? frame_distance(timing.start, placed_frame) : 0U;
    } else {
        clip_time.frame = at_show_frame;
    }
    // Native-rate videos resample exact replicated show time. Image sequences
    // have no rate and show one source frame per show frame.
    const auto elapsed = source_rate.has_value()
        ? media_frame_within(clip_time, *source_rate, sequence_count)
        : static_cast<core::u32>(clip_time.frame % sequence_count);
    const auto trim = static_cast<core::u32>(
        static_cast<core::u64>(timing.source_in) % sequence_count);
    return sequence_start
        + static_cast<core::u32>(
            (static_cast<core::u64>(trim) + elapsed) % sequence_count);
}

[[nodiscard]] std::optional<SceneId> read_scene_id(
    Reader& reader,
    const doc::PropertyMap& properties) noexcept {
    if (find(properties, schema::scene_id) == nullptr) {
        return std::nullopt;
    }
    const auto value = reader.integer(schema::scene_id, -1);
    if (value < 0) {
        reader.fail();
        return std::nullopt;
    }
    return SceneId{static_cast<core::u64>(value)};
}

// Every scene-scoped node names its Stage; a node without one is refused.
[[nodiscard]] SceneId read_required_scene_id(
    Reader& reader,
    const doc::PropertyMap& properties) noexcept {
    if (find(properties, schema::scene_id) == nullptr) {
        reader.fail();
        return SceneId{};
    }
    return read_scene_id(reader, properties).value_or(SceneId{});
}

[[nodiscard]] std::optional<doc::NodeId> read_parent_group(
    Reader& reader,
    const doc::PropertyMap& properties) noexcept {
    if (find(properties, schema::parent_group) == nullptr) {
        return std::nullopt;
    }
    const auto value = reader.integer(schema::parent_group, -1);
    if (value < 0) {
        reader.fail();
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(value)};
}

[[nodiscard]] std::string lens_calibration_property(
    core::u32 index, std::string_view suffix) {
    return std::string{schema::lens_calibration_prefix}
        + std::to_string(index) + std::string{suffix};
}

[[nodiscard]] Material read_material(Reader& reader, Material fallback) noexcept {
    Material material = fallback;
    material.base_color = reader.vector(
        schema::color_r, schema::color_g, schema::color_b,
        material.base_color);
    material.roughness = std::clamp(
        reader.number(schema::roughness, material.roughness), 0.0F, 1.0F);
    material.metallic = std::clamp(
        reader.number(schema::metallic, material.metallic), 0.0F, 1.0F);
    material.emissive = reader.vector(
        schema::emissive_r, schema::emissive_g, schema::emissive_b,
        material.emissive);
    material.emissive_strength = std::clamp(
        reader.number(schema::emissive_strength, material.emissive_strength),
        0.0F, 100'000.0F);
    material.normal_scale = std::clamp(
        reader.number(schema::normal_scale, material.normal_scale), 0.0F,
        4.0F);
    material.occlusion_strength = std::clamp(
        reader.number(schema::occlusion_strength,
                      material.occlusion_strength),
        0.0F, 1.0F);
    const auto alpha = reader.text(schema::alpha_mode, schema::alpha_opaque);
    if (alpha == schema::alpha_opaque) {
        material.alpha_mode = MaterialAlphaMode::Opaque;
    } else if (alpha == schema::alpha_mask) {
        material.alpha_mode = MaterialAlphaMode::Mask;
    } else if (alpha == schema::alpha_blend) {
        material.alpha_mode = MaterialAlphaMode::Blend;
    } else {
        reader.fail();
    }
    material.alpha_cutoff = std::clamp(
        reader.number(schema::alpha_cutoff, material.alpha_cutoff), 0.0F,
        1.0F);
    material.double_sided =
        reader.boolean(schema::double_sided, material.double_sided);
    material.base_color_texture = std::string{reader.text(
        schema::base_color_texture, material.base_color_texture)};
    material.metallic_roughness_texture = std::string{reader.text(
        schema::metallic_roughness_texture,
        material.metallic_roughness_texture)};
    material.normal_texture = std::string{
        reader.text(schema::normal_texture, material.normal_texture)};
    material.occlusion_texture = std::string{
        reader.text(schema::occlusion_texture, material.occlusion_texture)};
    material.emissive_texture = std::string{
        reader.text(schema::emissive_texture, material.emissive_texture)};
    material.base_alpha = std::clamp(reader.number(schema::base_alpha, material.base_alpha), 0.0F, 1.0F);
    material.clearcoat = std::clamp(reader.number(schema::clearcoat, material.clearcoat), 0.0F, 1.0F);
    material.clearcoat_roughness = std::clamp(reader.number(schema::clearcoat_roughness, material.clearcoat_roughness), 0.0F, 1.0F);
    material.clearcoat_normal_scale = std::clamp(reader.number(schema::clearcoat_normal_scale, material.clearcoat_normal_scale), 0.0F, 4.0F);
    material.sheen_roughness = std::clamp(reader.number(schema::sheen_roughness, material.sheen_roughness), 0.0F, 1.0F);
    material.transmission = std::clamp(reader.number(schema::transmission, material.transmission), 0.0F, 1.0F);
    material.ior = std::clamp(reader.number(schema::ior, material.ior), 1.0F, 3.0F);
    material.specular = std::clamp(reader.number(schema::specular, material.specular), 0.0F, 1.0F);
    material.anisotropy_strength = std::clamp(reader.number(schema::anisotropy_strength, material.anisotropy_strength), 0.0F, 1.0F);
    material.anisotropy_rotation = std::clamp(reader.number(schema::anisotropy_rotation, material.anisotropy_rotation), -6.283185307F, 6.283185307F);
    material.sheen_color.x = std::clamp(reader.number(schema::sheen_color_r, material.sheen_color.x), 0.0F, 1.0F);
    material.sheen_color.y = std::clamp(reader.number(schema::sheen_color_g, material.sheen_color.y), 0.0F, 1.0F);
    material.sheen_color.z = std::clamp(reader.number(schema::sheen_color_b, material.sheen_color.z), 0.0F, 1.0F);
    material.specular_color.x = std::clamp(reader.number(schema::specular_color_r, material.specular_color.x), 0.0F, 1.0F);
    material.specular_color.y = std::clamp(reader.number(schema::specular_color_g, material.specular_color.y), 0.0F, 1.0F);
    material.specular_color.z = std::clamp(reader.number(schema::specular_color_b, material.specular_color.z), 0.0F, 1.0F);
    material.clearcoat_texture = std::string{reader.text(schema::clearcoat_texture, material.clearcoat_texture)};
    material.clearcoat_roughness_texture = std::string{reader.text(schema::clearcoat_roughness_texture, material.clearcoat_roughness_texture)};
    material.clearcoat_normal_texture = std::string{reader.text(schema::clearcoat_normal_texture, material.clearcoat_normal_texture)};
    material.sheen_color_texture = std::string{reader.text(schema::sheen_color_texture, material.sheen_color_texture)};
    material.sheen_roughness_texture = std::string{reader.text(schema::sheen_roughness_texture, material.sheen_roughness_texture)};
    material.transmission_texture = std::string{reader.text(schema::transmission_texture, material.transmission_texture)};
    material.specular_texture = std::string{reader.text(schema::specular_texture, material.specular_texture)};
    material.specular_color_texture = std::string{reader.text(schema::specular_color_texture, material.specular_color_texture)};
    material.anisotropy_texture = std::string{reader.text(schema::anisotropy_texture, material.anisotropy_texture)};
    return material;
}

// A Background surface's fill, or nothing for an ordinary surface. Numbers
// out of range clamp, as every material factor does; an unknown mode word is
// a damaged node.
[[nodiscard]] std::optional<Fill> read_fill(
    Reader& reader, const doc::PropertyMap& properties) noexcept {
    if (find(properties, schema::fill_mode) == nullptr) {
        return std::nullopt;
    }
    Fill fill{};
    const auto mode = reader.text(schema::fill_mode, schema::solid_fill);
    if (mode == schema::solid_fill) {
        fill.mode = FillMode::Solid;
    } else if (mode == schema::linear_fill) {
        fill.mode = FillMode::Linear;
    } else if (mode == schema::radial_fill) {
        fill.mode = FillMode::Radial;
    } else {
        reader.fail();
    }
    const auto read_color = [&reader](FillColor& color, std::string_view r,
                                      std::string_view g, std::string_view b,
                                      std::string_view a) noexcept {
        color.r = std::clamp(reader.number(r, color.r), 0.0F, 1.0F);
        color.g = std::clamp(reader.number(g, color.g), 0.0F, 1.0F);
        color.b = std::clamp(reader.number(b, color.b), 0.0F, 1.0F);
        color.a = std::clamp(reader.number(a, color.a), 0.0F, 1.0F);
    };
    read_color(fill.color_a, schema::fill_a_r, schema::fill_a_g,
               schema::fill_a_b, schema::fill_a_a);
    read_color(fill.color_b, schema::fill_b_r, schema::fill_b_g,
               schema::fill_b_b, schema::fill_b_a);
    const float angle = reader.number(schema::fill_angle_turns, fill.angle);
    fill.angle = angle - std::floor(angle);
    fill.center_x = std::clamp(reader.number(schema::fill_center_x,
                                             fill.center_x),
                               -max_fill_center, max_fill_center);
    fill.center_y = std::clamp(reader.number(schema::fill_center_y,
                                             fill.center_y),
                               -max_fill_center, max_fill_center);
    fill.size = std::clamp(reader.number(schema::fill_size, fill.size),
                           0.01F, max_fill_size);
    return fill;
}

[[nodiscard]] LightUnit read_light_unit(Reader& reader) noexcept {
    const auto unit = reader.text(schema::light_unit, schema::unitless_unit);
    if (unit == schema::unitless_unit) {
        return LightUnit::Unitless;
    }
    if (unit == schema::lumens_unit) {
        return LightUnit::Lumens;
    }
    if (unit == schema::candela_unit) {
        return LightUnit::Candela;
    }
    if (unit == schema::lux_unit) {
        return LightUnit::Lux;
    }
    if (unit == schema::nits_unit) {
        return LightUnit::Nits;
    }
    reader.fail();
    return LightUnit::Unitless;
}

[[nodiscard]] LayerStyle read_style(Reader& reader) noexcept {
    LayerStyle style{};
    style.opacity = std::clamp(
        reader.number(schema::opacity, style.opacity), 0.0F, 1.0F);
    const float blend = reader.number(schema::blend_mode, 0.0F);
    const auto blend_index = static_cast<int>(blend);
    if (blend != static_cast<float>(blend_index) || blend_index < 0 ||
        blend_index > 3) {
        reader.fail();
    } else {
        style.blend = static_cast<BlendMode>(blend_index);
    }
    style.effects.brightness_enabled =
        reader.boolean(schema::brightness_enabled, false);
    style.effects.brightness = std::clamp(
        reader.number(schema::brightness, style.effects.brightness), 0.0F,
        16.0F);
    style.effects.blur_enabled =
        reader.boolean(schema::blur_enabled, false);
    style.effects.blur_radius = std::clamp(
        reader.number(schema::blur_radius, style.effects.blur_radius), 0.0F,
        64.0F);
    return style;
}

} // namespace

std::optional<Fill> read_surface_fill(
    const doc::PropertyMap& properties) noexcept {
    Reader reader{properties};
    const std::optional<Fill> fill = read_fill(reader, properties);
    return reader.ok() ? fill : std::nullopt;
}

core::u32 SceneCounters::duplicate_singletons() const noexcept {
    return duplicate_cameras;
}

core::u32 SceneCounters::over_capacity() const noexcept {
    const auto over = [](core::u32 held, core::u32 cap) noexcept -> core::u32 {
        return held > cap ? held - cap : 0U;
    };
    return over(objects, max_scene_instances)
        + over(media, max_scene_media)
        + over(lanes, max_scene_lanes)
        + over(groups, max_scene_groups)
        + over(models, max_scene_models)
        + over(particle_effects, max_scene_particle_effects)
        + over(directional_lights, max_scene_directional_lights)
        + over(point_lights, max_scene_point_lights)
        + over(slices, max_scene_slices)
        + over(channels, max_scene_channels)
        + over(effects, max_scene_effects)
        + over(masks, max_scene_masks)
        + over(mask_points, max_scene_mask_points);
}

void SceneModel::note(const Entry& entry, bool added) noexcept {
    const auto step = [added](core::u32& counter) noexcept {
        if (added) {
            ++counter;
        } else if (counter != 0U) {
            --counter;
        }
    };
    if (std::holds_alternative<StageEntry>(entry)) {
        step(counters_.stages);
        if (std::get<StageEntry>(entry).environment.physical_sky.enabled) step(counters_.directional_lights);
    } else if (std::holds_alternative<CameraEntry>(entry)) {
        step(counters_.cameras);
    } else if (std::holds_alternative<ObjectEntry>(entry)) {
        step(counters_.objects);
    } else if (std::holds_alternative<LaneEntry>(entry)) {
        step(counters_.lanes);
    } else if (std::holds_alternative<MarkerEntry>(entry)) {
        step(counters_.markers);
    } else if (std::holds_alternative<GroupEntry>(entry)) {
        step(counters_.groups);
    } else if (std::holds_alternative<MediaEntry>(entry)) {
        step(counters_.media);
    } else if (std::holds_alternative<ModelEntry>(entry)) {
        step(counters_.models);
    } else if (std::holds_alternative<ParticleEffectEntry>(entry)) {
        step(counters_.particle_effects);
    } else if (const auto* const light = std::get_if<LightEntry>(&entry)) {
        step(light->directional ? counters_.directional_lights
                                : counters_.point_lights);
    } else if (std::holds_alternative<ChannelEntry>(entry)) {
        step(counters_.channels);
    } else if (std::holds_alternative<EffectEntry>(entry)) {
        step(counters_.effects);
    } else if (std::holds_alternative<MaskEntry>(entry)) {
        step(counters_.masks);
    } else if (std::holds_alternative<MaskPointEntry>(entry)) {
        step(counters_.mask_points);
    } else if (std::holds_alternative<SliceEntry>(entry)) {
        step(counters_.slices);
    }
}

void SceneModel::forget(doc::NodeId id) noexcept {
    wall_colors_.erase(id);
    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        return;
    }
    note(found->second, false);
    entries_.erase(found);
}

void SceneModel::put(const doc::Node& node) {
    if (node.type == "output.surface") {
        forget(node.id);
        Reader reader{node.properties};
        WallColorSettings color;
        color.enabled = reader.boolean(schema::wall_color_enabled, false);
        color.camera_inverse_lut_source = reader.text(schema::camera_inverse_lut, "");
        for (std::size_t i = 0; i < color.primaries.size(); ++i)
            color.primaries[i] = reader.number(schema::wall_primaries[i], color.primaries[i]);
        color = clamped(color);
        if (reader.ok() && color != WallColorSettings{}) wall_colors_.emplace(node.id, std::move(color));
        return;
    }
    // A node whose type is not a scene type is not this model's business, but
    // it may have *been* one a moment ago: retyping a node from scene.object
    // to something else has to remove the object. So the old entry goes first,
    // whatever the new type turns out to be.
    forget(node.id);

    doc::PropertyMap light_properties;
    if (node.type == schema::light_type) {
        if (const auto p=linked_.occurrences.find(node.id); p!=linked_.occurrences.end()) {
            light_properties=node.properties;
            for (const auto& [key,value] : p->second.light) light_properties[key]=value;
        }
    }
    Reader reader{light_properties.empty() ? node.properties : light_properties};
    std::optional<Entry> parsed;

    if (node.type == schema::stage_type) {
        StageEntry stage{};
        stage.asset_refresh_generation = static_cast<core::u64>(std::max<core::i64>(0, reader.integer(schema::asset_refresh_generation, 0)));
        stage.asset_commit = {static_cast<core::u64>(reader.integer(schema::asset_commit_digest, 0)), static_cast<core::u64>(reader.integer(schema::asset_commit_control, 0)), static_cast<core::u64>(reader.integer(schema::asset_commit_show, 0))};
        stage.publication_revision = reader.text(publication::revision, "");
        stage.name = std::string{reader.text(schema::scene_name, "")};
        core::u32 width{};
        core::u32 height{};
        if (!as_u32(reader.integer(schema::canvas_width, 1'920), width)
            || !as_u32(reader.integer(schema::canvas_height, 1'080), height)) {
            reader.fail();
        }
        stage.canvas = Canvas{width, height};
        if (!valid(stage.canvas)) {
            reader.fail();
        }
        const Environment defaults{};
        stage.environment.sky_color = reader.vector(
            schema::sky_r, schema::sky_g, schema::sky_b, defaults.sky_color);
        stage.environment.ground_color = reader.vector(
            schema::ground_r,
            schema::ground_g,
            schema::ground_b,
            defaults.ground_color);
        stage.environment.ambient = reader.number(schema::ambient, defaults.ambient);
        stage.environment.map_source = std::string{reader.text(
            schema::environment_map, defaults.map_source)};
        stage.environment.map_intensity = std::clamp(
            reader.number(schema::environment_map_intensity,
                          defaults.map_intensity),
            0.0F, 100'000.0F);
        stage.environment.map_rotation = reader.number(
            schema::environment_map_rotation, defaults.map_rotation);
        auto& sky = stage.environment.physical_sky;
        sky.enabled = reader.boolean(schema::sky_physical_enabled, false);
        sky.time_of_day = reader.number(schema::sky_time_of_day, sky.time_of_day);
        sky.latitude = reader.number(schema::sky_latitude, sky.latitude);
        sky.north_rotation = reader.number(schema::sky_north_rotation, sky.north_rotation);
        sky.haze = reader.number(schema::sky_haze, sky.haze);
        sky.solar_illuminance = reader.number(schema::sky_solar_illuminance, sky.solar_illuminance);
        sky.angular_radius = reader.number(schema::sky_angular_radius, sky.angular_radius);
        sky.ground_albedo = reader.number(schema::sky_ground_albedo, sky.ground_albedo);
        sky = clamped(sky);
        // Clamped on the way in, like the map intensity above: a document is
        // replicated state and a value that only one node rejected would light
        // its share of the wall differently.
        stage.environment.fog_density = std::clamp(
            reader.number(schema::environment_fog_density,
                          defaults.fog_density),
            0.0F, max_fog_density);
        stage.environment.fog_color = reader.vector(
            schema::fog_r, schema::fog_g, schema::fog_b, defaults.fog_color);
        stage.environment.fog_anisotropy = std::clamp(
            reader.number(schema::environment_fog_anisotropy,
                          defaults.fog_anisotropy),
            -max_fog_anisotropy, max_fog_anisotropy);
        stage.environment.fog_height = std::clamp(
            reader.number(schema::environment_fog_height,
                          defaults.fog_height),
            -max_fog_height, max_fog_height);
        stage.environment.fog_height_falloff = std::clamp(
            reader.number(schema::environment_fog_height_falloff,
                          defaults.fog_height_falloff),
            0.0F, max_fog_height_falloff);
        stage.environment.fog_background_distance = std::clamp(
            reader.number(schema::environment_fog_background_distance,
                          defaults.fog_background_distance),
            0.0F, max_fog_background_distance);
        auto& baked = stage.environment.baked;
        baked.enabled = reader.boolean(schema::baked_enabled, baked.enabled);
        baked.minimum.x = reader.number(schema::baked_min_x, baked.minimum.x);
        baked.minimum.y = reader.number(schema::baked_min_y, baked.minimum.y);
        baked.minimum.z = reader.number(schema::baked_min_z, baked.minimum.z);
        baked.maximum.x = reader.number(schema::baked_max_x, baked.maximum.x);
        baked.maximum.y = reader.number(schema::baked_max_y, baked.maximum.y);
        baked.maximum.z = reader.number(schema::baked_max_z, baked.maximum.z);
        baked.count_x = static_cast<core::u32>(std::clamp<core::i64>(reader.integer(schema::baked_count_x, baked.count_x), 0, 65536));
        baked.count_y = static_cast<core::u32>(std::clamp<core::i64>(reader.integer(schema::baked_count_y, baked.count_y), 0, 65536));
        baked.count_z = static_cast<core::u32>(std::clamp<core::i64>(reader.integer(schema::baked_count_z, baked.count_z), 0, 65536));
        baked.samples = static_cast<core::u32>(std::clamp<core::i64>(reader.integer(schema::baked_samples, baked.samples), 0, 65536));
        baked.bounces = static_cast<core::u32>(std::clamp<core::i64>(reader.integer(schema::baked_bounces, baked.bounces), 0, 65536));
        baked.strength = reader.number(schema::baked_strength, baked.strength);
        baked.source = std::string{reader.text(schema::baked_source, baked.source)};
        baked.content = std::string{reader.text(schema::baked_content, baked.content)};
        baked.input_revision = std::string{reader.text(schema::baked_input_revision, "")};
        for (std::size_t i = 0; i < baked.reflections.size(); ++i) {
            auto& p = baked.reflections[i];
            const auto prefix = "reflection_" + std::to_string(i) + "_";
            p.source = doc::NodeId{static_cast<core::u64>(reader.integer(prefix + "node", 0))};
            p.enabled = reader.boolean(prefix + "enabled", false);
            p.sphere = reader.boolean(prefix + "sphere", false);
            p.position = reader.vector(prefix + "x", prefix + "y", prefix + "z", p.position);
            p.extent = reader.vector(prefix + "extent_x", prefix + "extent_y", prefix + "extent_z", p.extent);
            p.blend = reader.number(prefix + "blend", p.blend);
            p.intensity = reader.number(prefix + "intensity", p.intensity);
        }
        auto& planar = stage.environment.planar;
        planar.enabled = reader.boolean("planar_enabled", false);
        planar.receiver = doc::NodeId{static_cast<core::u64>(reader.integer("planar_receiver", 0))};
        planar.point = reader.vector("planar_x", "planar_y", "planar_z", planar.point);
        planar.normal = reader.vector("planar_nx", "planar_ny", "planar_nz", planar.normal);
        planar.strength = reader.number("planar_strength", planar.strength);
        planar = clamped(planar);
        baked = clamped(baked);
        parsed = Entry{stage};
    } else if (node.type == schema::camera_type) {
        CameraEntry camera{};
        camera.scene = read_required_scene_id(reader, node.properties);
        const Camera defaults{};
        // Placed like every other element. Absent placement is the default
        // camera's own.
        const Vec3 default_rotation = camera_rotation(defaults);
        camera.position = reader.vector(schema::position_x, schema::position_y,
                                        schema::position_z, defaults.eye);
        camera.rotation =
            Vec3{reader.number(schema::rotation_yaw, default_rotation.x),
                 reader.number(schema::rotation_pitch, default_rotation.y),
                 reader.number(schema::rotation_roll, default_rotation.z)};
        place_camera(camera.camera, camera.position, camera.rotation.x,
                     camera.rotation.y, camera.rotation.z);
        camera.camera.field_of_view =
            reader.number(schema::fov_turns, defaults.field_of_view);
        camera.tracking_source = doc::NodeId{static_cast<core::u64>(
            std::max(reader.integer(schema::camera_tracking_source, 0),
                     std::int64_t{0}))};
        {
            // Absent plate settings are the built-in defaults.
            const IcvfxCameraSettings icvfx_defaults{};
            IcvfxCameraSettings icvfx{};
            icvfx.inner_frustum =
                reader.boolean(schema::camera_inner_frustum, false);
            core::u32 camera_margin{};
            core::u32 camera_soft_edge{};
            core::u32 camera_numerator{};
            core::u32 camera_denominator{};
            if (!as_u32(reader.integer(
                            schema::camera_inner_frustum_margin_px,
                            static_cast<std::int64_t>(
                                icvfx_defaults.margin_px)),
                        camera_margin) ||
                !as_u32(reader.integer(
                            schema::camera_inner_frustum_soft_edge_px,
                            static_cast<std::int64_t>(
                                icvfx_defaults.soft_edge_px)),
                        camera_soft_edge) ||
                !as_u32(reader.integer(
                            schema::camera_inner_frustum_scale_numerator,
                            static_cast<std::int64_t>(
                                icvfx_defaults.resolution_numerator)),
                        camera_numerator) ||
                !as_u32(reader.integer(
                            schema::camera_inner_frustum_scale_denominator,
                            static_cast<std::int64_t>(
                                icvfx_defaults.resolution_denominator)),
                        camera_denominator)) {
                reader.fail();
            }
            icvfx.margin_px = camera_margin;
            icvfx.soft_edge_px = camera_soft_edge;
            icvfx.resolution_numerator = camera_numerator;
            icvfx.resolution_denominator = camera_denominator;
            const std::int64_t order =
                reader.integer(schema::camera_render_order, 0);
            icvfx.render_order = static_cast<core::i32>(std::clamp<
                std::int64_t>(order,
                              std::numeric_limits<core::i32>::min(),
                              std::numeric_limits<core::i32>::max()));
            camera.icvfx = clamped(icvfx);
        }
        camera.camera.near_plane =
            reader.number(schema::near_plane, defaults.near_plane);
        camera.camera.far_plane =
            reader.number(schema::far_plane, defaults.far_plane);
        camera.camera.aperture_f_number = reader.number(
            schema::aperture_f_number, defaults.aperture_f_number);
        camera.camera.focus_distance_m = std::clamp(reader.number(
            schema::focus_distance_m, defaults.focus_distance_m), 0.0F, 100000.0F);
        camera.camera.shutter_seconds = reader.number(
            schema::shutter_seconds, defaults.shutter_seconds);
        camera.camera.iso = reader.number(schema::iso, defaults.iso);
        camera.camera.exposure_compensation_ev = reader.number(
            schema::exposure_compensation_ev,
            defaults.exposure_compensation_ev);
        if (reader.boolean(schema::lens_profile_enabled, false)) {
            LensProfile profile{};
            profile.sensor_width_mm = reader.number(
                schema::lens_sensor_width_mm, profile.sensor_width_mm);
            profile.sensor_height_mm = reader.number(
                schema::lens_sensor_height_mm, profile.sensor_height_mm);
            profile.focal_length_mm = reader.number(
                schema::lens_focal_length_mm, profile.focal_length_mm);
            profile.entrance_pupil_offset_m = reader.number(
                schema::lens_entrance_pupil_offset_m,
                profile.entrance_pupil_offset_m);
            profile.distortion.k1 = reader.number(schema::lens_distortion_k1,
                                                   profile.distortion.k1);
            profile.distortion.k2 = reader.number(schema::lens_distortion_k2,
                                                   profile.distortion.k2);
            profile.distortion.k3 = reader.number(schema::lens_distortion_k3,
                                                   profile.distortion.k3);
            profile.distortion.p1 = reader.number(schema::lens_distortion_p1,
                                                   profile.distortion.p1);
            profile.distortion.p2 = reader.number(schema::lens_distortion_p2,
                                                   profile.distortion.p2);
            core::u32 calibration_count{};
            if (!as_u32(reader.integer(
                            schema::lens_calibration_count, 0),
                        calibration_count)) {
                reader.fail();
            }
            profile.calibration_count = std::min(
                calibration_count, max_lens_calibration_points);
            for (core::u32 index = 0U; index < profile.calibration_count;
                 ++index) {
                LensCalibrationPoint& point = profile.calibration[index];
                point.zoom_encoder = reader.number(
                    lens_calibration_property(
                        index, schema::lens_calibration_zoom_suffix),
                    point.zoom_encoder);
                point.focus_encoder = reader.number(
                    lens_calibration_property(
                        index, schema::lens_calibration_focus_suffix),
                    point.focus_encoder);
                point.focal_length_mm = reader.number(
                    lens_calibration_property(
                        index, schema::lens_calibration_focal_suffix),
                    profile.focal_length_mm);
                point.entrance_pupil_offset_m = reader.number(
                    lens_calibration_property(
                        index, schema::lens_calibration_pupil_suffix),
                    profile.entrance_pupil_offset_m);
                point.distortion.k1 = reader.number(
                    lens_calibration_property(
                        index, schema::lens_calibration_k1_suffix),
                    profile.distortion.k1);
                point.distortion.k2 = reader.number(
                    lens_calibration_property(
                        index, schema::lens_calibration_k2_suffix),
                    profile.distortion.k2);
                point.distortion.k3 = reader.number(
                    lens_calibration_property(
                        index, schema::lens_calibration_k3_suffix),
                    profile.distortion.k3);
                point.distortion.p1 = reader.number(
                    lens_calibration_property(
                        index, schema::lens_calibration_p1_suffix),
                    profile.distortion.p1);
                point.distortion.p2 = reader.number(
                    lens_calibration_property(
                        index, schema::lens_calibration_p2_suffix),
                    profile.distortion.p2);
            }
            camera.camera.lens_profile = clamped(profile);
        }
        camera.orbit_turns_per_second =
            reader.number(schema::orbit_turns_per_second, 0.0F);
        if (!valid(camera.camera)) {
            reader.fail();
        }
        parsed = Entry{camera};
    } else if (node.type == schema::object_type) {
        ObjectEntry object{};
        object.scene = read_required_scene_id(reader, node.properties);
        object.parent_group = read_parent_group(reader, node.properties);
        object.group_order = reader.integer(schema::group_order, 0);
        auto primitive = Primitive::Box;
        const auto name =
            reader.text(schema::primitive, primitive_name(Primitive::Box));
        if (!parse_primitive(name, primitive)) {
            reader.fail();
        }
        core::u32 segments{};
        if (!as_u32(reader.integer(schema::segments, 16), segments)) {
            reader.fail();
        }
        object.mesh = normalized(MeshKey{primitive, segments});
        object.position = reader.vector(
            schema::position_x, schema::position_y, schema::position_z, Vec3{});
        object.yaw = reader.number(schema::rotation_yaw, 0.0F);
        object.pitch = reader.number(schema::rotation_pitch, 0.0F);
        object.roll = reader.number(schema::rotation_roll, 0.0F);
        object.scale = reader.vector(
            schema::scale_x,
            schema::scale_y,
            schema::scale_z,
            Vec3{1.0F, 1.0F, 1.0F});
        object.material = read_material(reader, Material{});
        object.material.fill = read_fill(reader, node.properties);
        // Transparency in the fill is the operator's intent to see through
        // it, so an opaque surface carrying one is blended rather than drawn
        // as a solid card with the alpha ignored.
        if (object.material.fill.has_value() &&
            translucent(*object.material.fill) &&
            object.material.alpha_mode == MaterialAlphaMode::Opaque) {
            object.material.alpha_mode = MaterialAlphaMode::Blend;
        }
        object.style = read_style(reader);
        object.spin_turns_per_second =
            reader.number(schema::spin_turns_per_second, 0.0F);
        object.bob_amplitude = reader.number(schema::bob_amplitude, 0.0F);
        object.bob_turns_per_second =
            reader.number(schema::bob_turns_per_second, 0.0F);
        object.bake_static = reader.boolean(schema::bake_static, true);
        object.locked = reader.boolean(schema::locked, false);
        object.visible = reader.boolean(schema::visible, true);
        parsed = Entry{object};
    } else if (node.type == schema::group_type) {
        GroupEntry group{};
        group.scene = read_required_scene_id(reader, node.properties);
        group.parent_group = read_parent_group(reader, node.properties);
        group.group_order = reader.integer(schema::group_order, 0);
        group.name = std::string{reader.text(schema::group_name, "Group")};
        if (group.name.empty()) {
            reader.fail();
        }
        group.position = reader.vector(
            schema::position_x, schema::position_y, schema::position_z,
            Vec3{});
        group.yaw = reader.number(schema::rotation_yaw, 0.0F);
        group.pitch = reader.number(schema::rotation_pitch, 0.0F);
        group.roll = reader.number(schema::rotation_roll, 0.0F);
        group.scale = reader.vector(
            schema::scale_x, schema::scale_y, schema::scale_z,
            Vec3{1.0F, 1.0F, 1.0F});
        group.locked = reader.boolean(schema::locked, false);
        group.visible = reader.boolean(schema::visible, true);
        const auto kind = reader.text(schema::group_kind,
                                      schema::group_kind_folder);
        if (kind == schema::group_kind_vehicle_process) {
            group.kind = GroupKind::VehicleProcess;
        } else if (kind != schema::group_kind_folder) {
            reader.fail();
        }
        parsed = Entry{std::move(group)};
    } else if (node.type == schema::settings_type || node.type == publication::settings_type) {
        SettingsEntry settings{};
        if (node.type == publication::settings_type)
            settings.publication_scene = SceneId{static_cast<core::u64>(reader.integer(schema::scene_id, 0))};
        EffectSettings values{};
        core::u32 taps{};
        core::u32 passes{};
        core::u32 directional_taps{};
        if (!as_u32(reader.integer(schema::blur_taps,
                                   static_cast<std::int64_t>(values.blur_taps)),
                    taps) ||
            !as_u32(reader.integer(
                        schema::blur_passes,
                        static_cast<std::int64_t>(values.blur_passes)),
                    passes) ||
            !as_u32(reader.integer(
                        schema::directional_blur_taps,
                        static_cast<std::int64_t>(
                            values.directional_blur_taps)),
                    directional_taps)) {
            reader.fail();
        }
        values.blur_taps = taps;
        values.blur_passes = passes;
        values.directional_blur_taps = directional_taps;
        values.blur_sigma_ratio =
            reader.number(schema::blur_sigma_ratio, values.blur_sigma_ratio);
        values.brightness_gamma =
            reader.number(schema::brightness_gamma, values.brightness_gamma);
        values.graph_texture_budget_gb = reader.number(
            schema::graph_texture_budget_gb, values.graph_texture_budget_gb);
        const auto curve =
            reader.text(schema::brightness_curve, schema::exposure_curve);
        if (curve == schema::gain_curve) {
            values.brightness_curve = BrightnessCurve::Gain;
        } else if (curve == schema::exposure_curve) {
            values.brightness_curve = BrightnessCurve::Exposure;
        } else {
            // An unknown curve is a document this build does not understand.
            // Refusing it is what keeps a newer project from silently
            // rendering with the wrong response on an older node.
            reader.fail();
        }
        // Clamped once, here, so nothing downstream re-guards the same range
        // and no two callers can disagree about what an out-of-range document
        // meant.
        settings.settings = clamped(values);
        RendererSettings renderer{};
        renderer.frame_budget_us = static_cast<core::u32>(std::clamp<std::int64_t>(
            reader.integer(schema::frame_budget_us, 0), 0, 1'000'000));
        const auto backend =
            reader.text(schema::renderer_backend, schema::basic_renderer);
        if (backend == schema::advanced_renderer) {
            renderer.backend = RendererBackend::Advanced;
        } else if (backend == schema::basic_renderer) {
            renderer.backend = RendererBackend::Basic;
        } else {
            reader.fail();
        }
        renderer.required = reader.boolean(schema::renderer_required, false);
        const auto quality = reader.text(
            schema::renderer_quality, schema::balanced_quality);
        if (quality == schema::interactive_quality) {
            renderer.quality = RendererQuality::Interactive;
        } else if (quality == schema::balanced_quality) {
            renderer.quality = RendererQuality::Balanced;
        } else if (quality == schema::cinematic_quality) {
            renderer.quality = RendererQuality::Cinematic;
        } else {
            reader.fail();
        }
        renderer.ray_tracing =
            reader.boolean(schema::ray_tracing_enabled, false);
        renderer.ddgi = reader.boolean(schema::ddgi_enabled, false);
        renderer.restir = reader.boolean(schema::restir_enabled, false);
        renderer.denoising =
            reader.boolean(schema::denoising_enabled, false);
        renderer.taau = reader.boolean(schema::taau_enabled, false);
        renderer.spatial_antialiasing =
            reader.boolean(schema::spatial_antialiasing_enabled, false);
        renderer.plate_depth_of_field = reader.boolean(schema::plate_depth_of_field, false);
        renderer.plate_dof_radius = static_cast<core::u32>(std::clamp<std::int64_t>(
            reader.integer(schema::plate_dof_radius, renderer.plate_dof_radius), 0, max_plate_dof_radius));
        renderer.motion_blur =
            reader.boolean(schema::motion_blur_enabled, false);
        renderer.motion_blur_radius = static_cast<core::u32>(std::clamp<std::int64_t>(
            reader.integer(schema::motion_blur_radius, renderer.motion_blur_radius),
            0, max_motion_blur_radius));
        core::u32 probes_x{};
        core::u32 probes_y{};
        core::u32 probes_z{};
        core::u32 rays{};
        core::u32 apron{};
        core::u32 cascades{};
        core::u32 jitter_period{};
        if (!as_u32(reader.integer(
                        schema::ddgi_probe_count_x,
                        static_cast<std::int64_t>(
                            renderer.ddgi_probe_count_x)),
                    probes_x) ||
            !as_u32(reader.integer(
                        schema::ddgi_probe_count_y,
                        static_cast<std::int64_t>(
                            renderer.ddgi_probe_count_y)),
                    probes_y) ||
            !as_u32(reader.integer(
                        schema::ddgi_probe_count_z,
                        static_cast<std::int64_t>(
                            renderer.ddgi_probe_count_z)),
                    probes_z) ||
            !as_u32(reader.integer(
                        schema::ddgi_rays_per_probe,
                        static_cast<std::int64_t>(
                            renderer.ddgi_rays_per_probe)),
                    rays) ||
            !as_u32(reader.integer(
                        schema::slice_apron_limit,
                        static_cast<std::int64_t>(
                            renderer.slice_apron_limit)),
                    apron) ||
            !as_u32(reader.integer(
                        schema::shadow_cascades,
                        static_cast<std::int64_t>(
                            renderer.shadow_cascades)),
                    cascades) ||
            !as_u32(reader.integer(
                        schema::taa_sequence_length,
                        static_cast<std::int64_t>(
                            renderer.taa_sequence_length)),
                    jitter_period)) {
            reader.fail();
        }
        renderer.taa_sequence_length = jitter_period;
        renderer.ddgi_probe_count_x = probes_x;
        renderer.ddgi_probe_count_y = probes_y;
        renderer.ddgi_probe_count_z = probes_z;
        renderer.ddgi_rays_per_probe = rays;
        renderer.slice_apron_limit = apron;
        renderer.shadow_cascades = cascades;
        renderer.shadow_blend_band = reader.number(schema::shadow_blend_band, renderer.shadow_blend_band);
        renderer.shadow_filter_radius = reader.number(schema::shadow_filter_radius, renderer.shadow_filter_radius);
        renderer.contact_shadow_length = reader.number(schema::contact_shadow_length, renderer.contact_shadow_length);
        renderer.contact_shadow_thickness = reader.number(schema::contact_shadow_thickness, renderer.contact_shadow_thickness);

        renderer.frustum_culling = reader.boolean(schema::frustum_culling, renderer.frustum_culling);
        renderer.geometry_instancing = reader.boolean(schema::geometry_instancing, renderer.geometry_instancing);
        renderer.model_lod = reader.boolean(schema::model_lod, renderer.model_lod);
        renderer.texture_streaming = reader.boolean(schema::texture_streaming, renderer.texture_streaming);
        renderer.texture_budget_mib = static_cast<core::u32>(std::clamp<std::int64_t>(
            reader.integer(schema::texture_budget_mib, renderer.texture_budget_mib), 1,
            static_cast<std::int64_t>(max_texture_budget_mib)));
        renderer.texture_mip_bias = static_cast<core::u32>(std::clamp<std::int64_t>(
            reader.integer(schema::texture_mip_bias, renderer.texture_mip_bias), 0, 12));
        renderer.texture_anisotropy = static_cast<core::u32>(std::clamp<std::int64_t>(
            reader.integer(schema::texture_anisotropy, renderer.texture_anisotropy), 1, 16));
        renderer.texture_lod_bias = reader.number(schema::texture_lod_bias, renderer.texture_lod_bias);
        renderer.photometric_scale = reader.number(
            schema::photometric_scale, renderer.photometric_scale);
        renderer.gi_strength =
            reader.number(schema::gi_strength, renderer.gi_strength);

        renderer.occlusion_radius = reader.number(
            schema::screen_occlusion_radius, renderer.occlusion_radius);
        renderer.occlusion_strength = reader.number(
            schema::screen_occlusion_strength, renderer.occlusion_strength);
        renderer.bloom_strength =
            reader.number(schema::bloom_strength, renderer.bloom_strength);
        // Colour at the output boundary. An unknown transform name is a
        // document this build does not understand, refused for the same
        // reason an unknown brightness curve is: a newer project must not
        // silently grade an older node's share of the wall differently.
        const auto transform = reader.text(
            schema::display_transform, schema::aces_display_transform);
        if (transform == schema::srgb_display_transform) {
            renderer.color.display_transform = DisplayTransform::Srgb;
        } else if (transform == schema::aces_display_transform) {
            renderer.color.display_transform =
                DisplayTransform::AcesApproximate;
        } else if (transform == schema::aces13_display_transform) {
            renderer.color.display_transform = DisplayTransform::Aces13;
        } else {
            reader.fail();
        }
        renderer.color.show_lut_source = std::string{
            reader.text(schema::show_lut, renderer.color.show_lut_source)};
        renderer.color.dither =
            reader.boolean(schema::output_dither, renderer.color.dither);
        renderer.icvfx.inner_frustum =
            reader.boolean(schema::inner_frustum_enabled, false);
        renderer.icvfx.pose_prediction = reader.boolean(
            schema::tracking_pose_prediction, renderer.icvfx.pose_prediction);
        if (!as_u32(reader.integer(schema::tracking_prediction_window,
                                  renderer.icvfx.prediction_window),
                    renderer.icvfx.prediction_window)
            || !as_u32(reader.integer(schema::tracking_jitter_filter_percent,
                                     renderer.icvfx.jitter_filter_percent),
                       renderer.icvfx.jitter_filter_percent)) {
            reader.fail();
        }
        core::u32 margin{};
        core::u32 soft_edge{};
        core::u32 latency{};
        core::u32 scale_numerator{};
        core::u32 scale_denominator{};
        if (!as_u32(reader.integer(
                        schema::inner_frustum_margin_px,
                        static_cast<std::int64_t>(
                            renderer.icvfx.camera_defaults.margin_px)),
                    margin) ||
            !as_u32(reader.integer(
                        schema::inner_frustum_soft_edge_px,
                        static_cast<std::int64_t>(
                            renderer.icvfx.camera_defaults.soft_edge_px)),
                    soft_edge) ||
            !as_u32(reader.integer(
                        schema::tracking_latency_frames,
                        static_cast<std::int64_t>(
                            renderer.icvfx.latency_frames)),
                    latency) ||
            !as_u32(reader.integer(
                        schema::inner_frustum_scale_numerator,
                        static_cast<std::int64_t>(
                            renderer.icvfx.camera_defaults
                                .resolution_numerator)),
                    scale_numerator) ||
            !as_u32(reader.integer(
                        schema::inner_frustum_scale_denominator,
                        static_cast<std::int64_t>(
                            renderer.icvfx.camera_defaults
                                .resolution_denominator)),
                    scale_denominator)) {
            reader.fail();
        }
        renderer.icvfx.camera_defaults.margin_px = margin;
        renderer.icvfx.camera_defaults.soft_edge_px = soft_edge;
        renderer.icvfx.latency_frames = latency;
        renderer.icvfx.camera_defaults.resolution_numerator = scale_numerator;
        renderer.icvfx.camera_defaults.resolution_denominator =
            scale_denominator;
        settings.renderer = clamped(renderer);
        parsed = Entry{settings};
    } else if (node.type == schema::tracking_source_type) {
        TrackingSourceEntry source{};
        source.name =
            std::string{reader.text(schema::tracking_source_name, "Tracker")};
        if (source.name.empty()) {
            reader.fail();
        }
        source.settings.enabled =
            reader.boolean(schema::tracking_enabled, false);
        const auto protocol =
            reader.text(schema::tracking_protocol, schema::freed_protocol);
        if (protocol == schema::freed_protocol) {
            source.settings.protocol = TrackerProtocolKind::FreeD;
        } else if (protocol == schema::opentrackio_protocol) {
            source.settings.protocol = TrackerProtocolKind::OpenTrackIO;
        } else if (protocol == schema::natnet_protocol) {
            source.settings.protocol = TrackerProtocolKind::NatNet;
        } else {
            // Tracking with a parser the project was not authored against is
            // worse than not tracking, so this is refused rather than guessed.
            reader.fail();
        }
        core::u32 source_port{};
        core::u32 source_camera{};
        if (!as_u32(reader.integer(schema::tracking_port, 0), source_port) ||
            !as_u32(reader.integer(schema::tracking_camera_id, 0),
                    source_camera)) {
            reader.fail();
        }
        source.settings.port = source_port;
        source.settings.camera_id = source_camera;
        core::u32 command_port{};
        if (!as_u32(reader.integer(
                        schema::tracking_command_port,
                        static_cast<std::int64_t>(
                            source.settings.command_port)),
                    command_port)) {
            reader.fail();
        }
        source.settings.command_port = command_port;
        // A group that does not parse is refused rather than defaulted: a
        // feed listening on the wrong group is silent in exactly the way a
        // missing setting is, and the operator would hunt the cable.
        const std::string default_group =
            multicast_group_text(source.settings.multicast_group);
        const auto group =
            reader.text(schema::tracking_multicast_group, default_group);
        if (!parse_multicast_group(group, source.settings.multicast_group)) {
            reader.fail();
        }
        source.settings.sensor_height_mm = reader.number(
            schema::tracking_sensor_height_mm,
            source.settings.sensor_height_mm);
        source.settings.sensor_aspect_ratio = reader.number(
            schema::tracking_sensor_aspect,
            source.settings.sensor_aspect_ratio);
        source.settings.fallback_focal_length_mm = reader.number(
            schema::tracking_fallback_focal_mm,
            source.settings.fallback_focal_length_mm);
        // Absent is stage-native: the tracker already reports stage space.
        // An unrecognised value is refused rather than guessed: tracking
        // through the wrong axis convention puts the plate somewhere confident
        // and wrong, which is worse than refusing to load.
        const auto axes =
            reader.text(schema::tracking_axes, schema::tracking_axes_stage);
        if (axes == schema::tracking_axes_stage) {
            source.settings.calibration.axes = TrackerAxes::StageNative;
        } else if (axes == schema::tracking_axes_z_up) {
            source.settings.calibration.axes = TrackerAxes::ZUpRightHanded;
        } else {
            reader.fail();
        }
        source.settings.calibration.origin = Vec3{
            reader.number(schema::tracking_origin_x, 0.0),
            reader.number(schema::tracking_origin_y, 0.0),
            reader.number(schema::tracking_origin_z, 0.0),
        };
        source.settings.calibration.heading =
            reader.number(schema::tracking_heading, 0.0);
        source.settings.calibration.scale =
            reader.number(schema::tracking_scale, 1.0);
        source.settings = clamped(source.settings);
        parsed = Entry{source};
    } else if (node.type == schema::timeline_lane_type) {
        LaneEntry lane{};
        lane.scene = read_required_scene_id(reader, node.properties);
        lane.order = reader.integer(schema::lane_order, 0);
        lane.name = std::string{reader.text(schema::lane_name, "Layer")};
        if (lane.name.empty()) {
            reader.fail();
        }
        lane.muted = reader.boolean(schema::lane_muted, false);
        lane.locked = reader.boolean(schema::lane_locked, false);
        lane.sync_locked = reader.boolean(schema::lane_sync_locked, false);
        const auto mode = reader.text(
            schema::lane_compositing_mode, schema::lane_normal_mode);
        if (mode == schema::lane_normal_mode) {
            lane.compositing = BlendMode::Normal;
        } else if (mode == schema::lane_add_mode) {
            lane.compositing = BlendMode::Add;
        } else if (mode == schema::lane_multiply_mode) {
            lane.compositing = BlendMode::Multiply;
        } else if (mode == schema::lane_screen_mode) {
            lane.compositing = BlendMode::Screen;
        } else {
            reader.fail();
        }
        // The screen. Read with the same property names a drawable uses, so a
        // track keyframes and inspects through the paths that already exist.
        lane.position = reader.vector(
            schema::position_x, schema::position_y, schema::position_z, Vec3{});
        lane.yaw = reader.number(schema::rotation_yaw, 0.0F);
        lane.pitch = reader.number(schema::rotation_pitch, 0.0F);
        lane.roll = reader.number(schema::rotation_roll, 0.0F);
        lane.scale = reader.vector(
            schema::scale_x, schema::scale_y, schema::scale_z,
            Vec3{1.0F, 1.0F, 1.0F});
        // Only opacity is read here. A track's blend is lane_compositing_mode
        // above and its effects are first-class effect nodes, so there is
        // exactly one place each of those is authored.
        lane.style.opacity = std::clamp(
            reader.number(schema::opacity, 1.0F), 0.0F, 1.0F);
        lane.style.blend = lane.compositing;
        core::u32 raster_width{};
        core::u32 raster_height{};
        if (!as_u32(reader.integer(schema::lane_raster_width,
                                   static_cast<core::i64>(
                                       default_raster_width)),
                    raster_width)
            || !as_u32(reader.integer(schema::lane_raster_height,
                                      static_cast<core::i64>(
                                          default_raster_height)),
                       raster_height)
            || raster_width == 0U || raster_height == 0U
            || raster_width > max_raster_extent
            || raster_height > max_raster_extent) {
            // A screen with no format is not a screen. Refusing the node makes
            // the mistake visible instead of quietly rendering at some default
            // the operator never chose.
            reader.fail();
        } else {
            lane.raster_width = raster_width;
            lane.raster_height = raster_height;
        }
        lane.aspect_locked = reader.boolean(schema::lane_aspect_locked, true);
        if (find(node.properties, schema::lane_kind) != nullptr) {
            if (reader.text(schema::lane_kind, "") == schema::lane_kind_vehicle) {
                lane.vehicle = true;
            } else {
                reader.fail();
            }
        }
        // A slot screen names its vehicle track and its slot together. One
        // without the other is a screen that belongs nowhere, and a vehicle
        // track is never itself a slot of another.
        const bool has_parent =
            find(node.properties, schema::lane_parent) != nullptr;
        const bool has_slot =
            find(node.properties, schema::lane_slot) != nullptr;
        if (has_parent != has_slot || (has_parent && lane.vehicle)) {
            reader.fail();
        } else if (has_parent) {
            const auto parent = reader.integer(schema::lane_parent, -1);
            const auto slot = std::find(
                schema::vehicle_slot_keys.begin(),
                schema::vehicle_slot_keys.end(),
                reader.text(schema::lane_slot, ""));
            if (parent <= 0 || slot == schema::vehicle_slot_keys.end()) {
                reader.fail();
            } else {
                lane.parent = doc::NodeId{static_cast<core::u64>(parent)};
                lane.slot = static_cast<core::u8>(
                    slot - schema::vehicle_slot_keys.begin());
            }
        }
        lane.time_offset = reader.integer(schema::lane_time_offset, 0);
        if (lane.time_offset < -max_lane_time_offset
            || lane.time_offset > max_lane_time_offset
            || (lane.time_offset != 0 && !has_parent)) {
            reader.fail();
        }
        parsed = Entry{std::move(lane)};
    } else if (node.type == schema::timeline_marker_type) {
        MarkerEntry marker{};
        marker.scene = read_required_scene_id(reader, node.properties);
        marker.frame = reader.integer(schema::marker_frame, 0);
        // A marker at a negative frame is not before the show, it is a
        // damaged node: the show has no time before frame zero to put it in.
        if (marker.frame < 0) {
            reader.fail();
        }
        marker.name = std::string{reader.text(schema::marker_name, "")};
        marker.note = std::string{reader.text(schema::marker_note, "")};
        if (marker.name.size() > max_timeline_marker_name_bytes
            || marker.note.size() > max_timeline_marker_note_bytes) {
            reader.fail();
        }
        const auto color = reader.integer(schema::marker_color, 0);
        if (color < 0 || color >= timeline_marker_colors) {
            reader.fail();
        } else {
            marker.color = static_cast<core::u8>(color);
        }
        const auto cue = reader.text(schema::marker_cue, schema::marker_cue_none);
        if (cue == schema::marker_cue_none) {
            marker.cue = CueAction::None;
        } else if (cue == schema::marker_cue_hold) {
            marker.cue = CueAction::Hold;
        } else if (cue == schema::marker_cue_loop) {
            marker.cue = CueAction::Loop;
        } else {
            reader.fail();
        }
        const auto wait = reader.integer(schema::marker_loop_wait, 0);
        const auto fade = reader.integer(schema::marker_loop_fade, 0);
        if (wait < 0 || wait > max_loop_wait_ms || fade < 0
            || fade > max_loop_fade_ms) {
            reader.fail();
        } else {
            marker.loop_wait_ms = static_cast<core::u32>(wait);
            marker.loop_fade_ms = static_cast<core::u32>(fade);
        }
        parsed = Entry{std::move(marker)};
    } else if (node.type == schema::media_type) {
        MediaEntry media{};
        media.scene = read_required_scene_id(reader, node.properties);
        media.parent_group = read_parent_group(reader, node.properties);
        media.group_order = reader.integer(schema::group_order, 0);
        media.source = std::string{reader.text(schema::source, "")};
        if (media.source.empty()) {
            // A clip with no sequence to load is a mistake, not an empty
            // default: refuse it so it reads as unauthored rather than blank.
            reader.fail();
        }
        if (find(node.properties, schema::asset_id) != nullptr) {
            const auto asset = reader.integer(schema::asset_id, -1);
            if (asset <= 0) {
                reader.fail();
            } else {
                media.asset =
                    doc::NodeId{static_cast<core::u64>(asset)};
            }
        }
        core::u32 start{};
        core::u32 count{};
        if (!as_u32(reader.integer(schema::sequence_start, 0), start)
            || !as_u32(reader.integer(schema::sequence_count, 1), count)) {
            reader.fail();
        }
        if (count == 0U) {
            reader.fail();
        }
        media.sequence_start = start;
        media.sequence_count = count;
        const bool has_rate_numerator =
            find(node.properties, schema::source_rate_numerator) != nullptr;
        const bool has_rate_denominator =
            find(node.properties, schema::source_rate_denominator) != nullptr;
        if (has_rate_numerator != has_rate_denominator) {
            reader.fail();
        } else if (has_rate_numerator) {
            core::u32 numerator{};
            core::u32 denominator{};
            if (!as_u32(
                    reader.integer(schema::source_rate_numerator, 0),
                    numerator) ||
                !as_u32(
                    reader.integer(schema::source_rate_denominator, 0),
                    denominator) ||
                numerator == 0U || denominator == 0U) {
                reader.fail();
            } else {
                media.frame_rate = MediaFrameRate{numerator, denominator};
            }
        }
        media.timing.start = reader.integer(schema::clip_start, 0);
        media.timing.duration = reader.integer(schema::clip_duration, 0);
        media.timing.source_in = reader.integer(schema::source_in, 0);
        media.timing.fade_in = reader.integer(schema::fade_in, 0);
        media.timing.fade_out = reader.integer(schema::fade_out, 0);
        if (find(node.properties, schema::lane_id) != nullptr) {
            const auto lane = reader.integer(schema::lane_id, -1);
            if (lane <= 0) {
                reader.fail();
            } else {
                media.lane = doc::NodeId{static_cast<core::u64>(lane)};
            }
        }
        media.clip_order = reader.integer(schema::clip_order, 0);
        if (!valid_clip_timing(media.timing)) {
            reader.fail();
        }
        media.position = reader.vector(
            schema::position_x, schema::position_y, schema::position_z, Vec3{});
        media.yaw = reader.number(schema::rotation_yaw, 0.0F);
        media.pitch = reader.number(schema::rotation_pitch, 0.0F);
        media.roll = reader.number(schema::rotation_roll, 0.0F);
        media.scale = reader.vector(
            schema::scale_x,
            schema::scale_y,
            schema::scale_z,
            Vec3{1.0F, 1.0F, 1.0F});
        media.style = read_style(reader);
        media.locked = reader.boolean(schema::locked, false);
        media.visible = reader.boolean(schema::visible, true);
        const auto resolution = reader.text(
            schema::media_resolution, schema::media_resolution_full);
        if (resolution == schema::media_resolution_half) {
            media.resolution = MediaResolution::Half;
        } else if (resolution != schema::media_resolution_full) {
            reader.fail();
        }
        // A take's plates. Read only from a clip that says it came from a
        // plateset, so an ordinary clip pays for none of these lookups.
        if (find(node.properties, schema::plateset_asset) != nullptr) {
            for (std::size_t slot = 0U; slot < vehicle_slot_count; ++slot) {
                const std::string source_name =
                    schema::plate_property(slot, schema::plate_source_suffix);
                if (find(node.properties, source_name) == nullptr) {
                    continue;
                }
                MediaEntry::Plate plate{};
                plate.source = std::string{reader.text(source_name, "")};
                const auto plate_asset = reader.integer(
                    schema::plate_property(slot, schema::plate_asset_suffix),
                    0);
                const auto plate_count = reader.integer(
                    schema::plate_property(slot, schema::plate_count_suffix),
                    0);
                plate.offset = reader.integer(
                    schema::plate_property(slot, schema::plate_offset_suffix),
                    0);
                // A plate with no file, no frames or a negative skip is a
                // damaged take, refused whole: playing the plates that did
                // parse would put a car on stage with a wall missing and
                // nothing to say why.
                if (plate.source.empty() || plate_asset < 0
                    || !as_u32(plate_count, plate.count) || plate.count == 0U
                    || plate.offset < 0
                    || plate.offset > max_lane_time_offset) {
                    reader.fail();
                    continue;
                }
                plate.asset =
                    doc::NodeId{static_cast<core::u64>(plate_asset)};
                media.plates.resize(vehicle_slot_count);
                media.plates[slot] = std::move(plate);
            }
        }
        parsed = Entry{media};
    } else if (node.type == schema::model_type) {
        ModelEntry model{};
        model.scene = read_required_scene_id(reader, node.properties);
        model.parent_group = read_parent_group(reader, node.properties);
        model.group_order = reader.integer(schema::group_order, 0);
        model.source = std::string{reader.text(schema::source, "")};
        const auto linked = linked_.occurrences.find(node.id);
        const auto batch_text = linked != linked_.occurrences.end() ? linked->second.batch
            : std::string{reader.text(instance_batch_property, "")};
        if (!batch_text.empty() && !decode_instance_batch(batch_text,model.batch)) reader.fail();
        if (linked != linked_.occurrences.end()) {
            model.source = linked->second.source;
            model.linked_world = linked->second.world;
            model.material_override_mask = linked->second.material_mask;
        }
        if (model.source.empty() && linked == linked_.occurrences.end()) {
            // A model with no file to load is unauthored, not an empty default.
            reader.fail();
        }
        model.position = reader.vector(
            schema::position_x, schema::position_y, schema::position_z, Vec3{});
        model.yaw = reader.number(schema::rotation_yaw, 0.0F);
        model.pitch = reader.number(schema::rotation_pitch, 0.0F);
        model.roll = reader.number(schema::rotation_roll, 0.0F);
        model.scale = reader.vector(
            schema::scale_x,
            schema::scale_y,
            schema::scale_z,
            Vec3{1.0F, 1.0F, 1.0F});
        model.material = read_material(reader, model.material);
        if (linked == linked_.occurrences.end()) {
            std::size_t affine_fields{};
            for (std::size_t i = 0; i < 16; ++i) {
                const auto key = std::string{schema::model_affine} + std::to_string(i);
                if (node.properties.contains(key)) ++affine_fields;
                model.affine.m[i] = reader.number(key, model.affine.m[i]);
            }
            if ((affine_fields != 0 && affine_fields != 16) || !valid_link_affine(model.affine)) reader.fail();
            if (reader.boolean(schema::model_factor_overrides, false)) {
                core::u32 bit = 1;
                for (const auto key : {schema::color_r,schema::color_g,schema::color_b,schema::roughness,
                        schema::metallic,schema::emissive_r,schema::emissive_g,schema::emissive_b}) {
                    if (node.properties.contains(key)) model.material_override_mask |= bit;
                    bit <<= 1U;
                }
            }
        }
        if (linked != linked_.occurrences.end()) model.material = linked->second.material;
        model.style = read_style(reader);
        model.bake_static = reader.boolean(schema::bake_static, true);
        model.animation.enabled = reader.boolean(schema::animation_enabled, true);
        model.animation.clip = static_cast<core::u32>(std::clamp<core::i64>(reader.integer(schema::animation_clip,0),0,63));
        model.animation.loop = reader.boolean(schema::animation_loop,true);
        model.animation.start_frame = static_cast<core::u64>(std::max<core::i64>(reader.integer(schema::animation_start_frame,0),0));
        model.animation.wind = reader.vector(schema::wind_x,schema::wind_y,schema::wind_z,{});
        model.animation.wind.x = std::clamp(model.animation.wind.x,-100.0F,100.0F);
        model.animation.wind.y = std::clamp(model.animation.wind.y,-100.0F,100.0F);
        model.animation.wind.z = std::clamp(model.animation.wind.z,-100.0F,100.0F);
        model.animation.wind_period_frames = static_cast<core::u32>(std::clamp<core::i64>(reader.integer(schema::wind_period_frames,120),2,1000000));
        if (linked != linked_.occurrences.end()) {
            model.animation=linked->second.animation;
            if (linked->second.animated || model.animation.wind != Vec3{}) model.bake_static=false;
        }
        model.locked = reader.boolean(schema::locked, false);
        model.visible = reader.boolean(schema::visible, true);
        if (linked != linked_.occurrences.end())
            model.visible = linked->second.visible && !linked->second.source.empty();
        parsed = Entry{std::move(model)};
    } else if (node.type == schema::particle_effect_type) {
        ParticleEffectEntry effect{};
        effect.scene = read_required_scene_id(reader, node.properties);
        effect.parent_group = read_parent_group(reader, node.properties);
        effect.group_order = reader.integer(schema::group_order, 0);
        effect.source = std::string{reader.text(schema::source, "")};
        if (effect.source.empty()) {
            // An effect with no package to load is unauthored, not empty.
            reader.fail();
        }
        effect.position = reader.vector(
            schema::position_x, schema::position_y, schema::position_z, Vec3{});
        effect.yaw = reader.number(schema::rotation_yaw, 0.0F);
        effect.pitch = reader.number(schema::rotation_pitch, 0.0F);
        effect.roll = reader.number(schema::rotation_roll, 0.0F);
        effect.scale = reader.vector(
            schema::scale_x, schema::scale_y, schema::scale_z, Vec3{1.0F, 1.0F, 1.0F});
        const auto seed = reader.integer(schema::particle_seed, 0);
        const auto start = reader.integer(schema::particle_start_frame, 0);
        // Out of range is a damaged node, not something to clamp: two nodes
        // must not quietly disagree about which particles they draw.
        if (seed < 0 || seed > static_cast<core::i64>(std::numeric_limits<core::u32>::max()) || start < 0) {
            reader.fail();
        } else {
            effect.seed = static_cast<core::u32>(seed);
            effect.start_frame = static_cast<core::u64>(start);
        }
        effect.locked = reader.boolean(schema::locked, false);
        effect.visible = reader.boolean(schema::visible, true);
        // Published parameter values. A malformed one damages the node like
        // any typed property: two nodes must not disagree about it.
        for (const auto& [name, value] : node.properties) {
            if (!std::string_view{name}.starts_with(schema::particle_parameter_prefix)) {
                continue;
            }
            const std::string_view key =
                std::string_view{name}.substr(schema::particle_parameter_prefix.size());
            const auto* const real = std::get_if<double>(&value);
            const auto* const whole = std::get_if<std::int64_t>(&value);
            const double number = real != nullptr ? *real
                : whole != nullptr ? static_cast<double>(*whole)
                : std::numeric_limits<double>::quiet_NaN();
            if (!parse_particle_parameter_key(key) || !std::isfinite(number)
                || std::abs(number) > static_cast<double>(std::numeric_limits<float>::max())
                || effect.parameters.size() >= max_particle_parameters) {
                reader.fail();
                break;
            }
            effect.parameters.emplace_back(std::string{key}, number);
        }
        std::sort(effect.parameters.begin(), effect.parameters.end());
        parsed = Entry{std::move(effect)};
    } else if (node.type == schema::light_type) {
        LightEntry light{};
        light.scene = read_required_scene_id(reader, node.properties);
        const auto kind = reader.text(schema::light_kind, schema::directional_kind);
        if (kind == schema::directional_kind) {
            light.directional = true;
            light.vector = reader.vector(
                schema::direction_x,
                schema::direction_y,
                schema::direction_z,
                Vec3{0.0F, -1.0F, 0.0F});
        } else if (kind == schema::point_kind || kind == schema::spot_kind
                   || kind == schema::rectangle_kind || kind == schema::sphere_kind) {
            light.directional = false;
            light.local_kind = kind == schema::spot_kind
                ? LocalLightKind::Spot
                : kind == schema::rectangle_kind
                ? LocalLightKind::Rectangle
                : kind == schema::sphere_kind ? LocalLightKind::Sphere : LocalLightKind::Point;
            light.vector = reader.vector(
                schema::position_x,
                schema::position_y,
                schema::position_z,
                Vec3{});
        } else {
            reader.fail();
        }
        light.color = reader.vector(
            schema::color_r, schema::color_g, schema::color_b, Vec3{1.0F, 1.0F, 1.0F});
        light.intensity = reader.number(schema::intensity, 1.0F);
        light.range = reader.number(schema::range, 10.0F);
        light.unit = read_light_unit(reader);
        light.direction = reader.vector(
            schema::direction_x, schema::direction_y, schema::direction_z,
            Vec3{0.0F, -1.0F, 0.0F});
        light.inner_cone = reader.number(schema::inner_cone_turns,
                                         light.inner_cone);
        light.outer_cone = reader.number(schema::outer_cone_turns,
                                         light.outer_cone);
        if (light.local_kind == LocalLightKind::Spot) {
            light.outer_cone = std::clamp(light.outer_cone, 0.001F, 0.249F);
            light.inner_cone = std::clamp(light.inner_cone, 0.0F, light.outer_cone);
        }
        light.width = std::clamp(reader.number(schema::light_width, light.width), 0.001F, 10000.0F);
        light.height = std::clamp(reader.number(schema::light_height, light.height), 0.001F, 10000.0F);
        light.radius = std::clamp(reader.number(schema::light_radius, light.radius), 0.001F, 10000.0F);
        light.area_rotation = std::clamp(reader.number(schema::area_rotation_turns, 0.0F), -1.0F, 1.0F);
        light.two_sided = reader.boolean(schema::two_sided, false);
        light.ies_source = reader.text(schema::ies_source, "");
        light.ies_rotation = std::clamp(reader.number(schema::ies_rotation_turns, 0.0F), -1.0F, 1.0F);
        if (light.ies_source.size() > 1024U || (!light.ies_source.empty()
            && (light.directional || light.local_kind == LocalLightKind::Rectangle || light.local_kind == LocalLightKind::Sphere))) reader.fail();
        light.angular_radius = reader.number(
            schema::angular_radius_turns, light.angular_radius);
        light.color_temperature_kelvin = std::clamp(
            reader.number(schema::color_temperature_kelvin,
                          light.color_temperature_kelvin),
            1'000.0F, 40'000.0F);
        light.use_color_temperature = reader.boolean(
            schema::use_color_temperature, false);
        light.casts_shadow = reader.boolean(schema::casts_shadow, false);
        const std::string_view tier =
            reader.text(schema::shadow_tier, schema::medium_tier);
        if (tier == schema::small_tier) {
            light.shadow_tier = ShadowTier::Small;
        } else if (tier == schema::large_tier) {
            light.shadow_tier = ShadowTier::Large;
        } else if (tier == schema::medium_tier) {
            light.shadow_tier = ShadowTier::Medium;
        } else {
            reader.fail();
        }
        if (!(light.intensity >= 0.0F) || !(light.range > 0.0F)
            || !(light.inner_cone >= 0.0F)
            || !(light.outer_cone >= light.inner_cone)
            || !(light.outer_cone < 0.5F) || !(light.width > 0.0F)
            || !(light.height > 0.0F) || !(light.angular_radius >= 0.0F)
            || !(light.angular_radius < 0.5F)) {
            reader.fail();
        }
        light.bake_indirect = reader.boolean(schema::bake_indirect, true);
        light.locked = reader.boolean(schema::locked, false);
        light.visible = reader.boolean(schema::visible, true);
        parsed = Entry{light};
    } else if (node.type == schema::slice_type) {
        SliceEntry slice{};
        core::u32 index{};
        core::u32 x{};
        core::u32 y{};
        core::u32 width{};
        core::u32 height{};
        if (!as_u32(reader.integer(schema::node_index, 0), index)
            || !as_u32(reader.integer(schema::slice_x, 0), x)
            || !as_u32(reader.integer(schema::slice_y, 0), y)
            || !as_u32(reader.integer(schema::slice_width, 0), width)
            || !as_u32(reader.integer(schema::slice_height, 0), height)) {
            reader.fail();
        }
        if (width == 0U || height == 0U) {
            reader.fail();
        }
        slice.binding = SliceBinding{index, CanvasRect{x, y, width, height}};
        parsed = Entry{slice};
    } else if (node.type == schema::effect_type) {
        EffectEntry effect{};
        const auto target = reader.integer(schema::effect_target, -1);
        if (target < 0) {
            reader.fail();
        } else {
            effect.target = doc::NodeId{static_cast<core::u64>(target)};
        }
        const auto kind = reader.text(schema::effect_kind, "");
        if (kind == schema::brightness_kind) {
            effect.kind = EffectKind::Brightness;
            effect.value = std::clamp(
                reader.number(schema::brightness, 1.0F), 0.0F, 16.0F);
        } else if (kind == schema::saturation_kind) {
            effect.kind = EffectKind::Saturation;
            effect.value = reader.bounded_number_or_identity(
                schema::saturation, 1.0F, 0.0F, max_saturation);
        } else if (kind == schema::blur_kind) {
            effect.kind = EffectKind::Blur;
            effect.value = std::clamp(
                reader.number(schema::blur_radius, 8.0F), 0.0F, 64.0F);
        } else if (kind == schema::box_blur_kind) {
            effect.kind = EffectKind::BoxBlur;
            core::u32 radius{};
            core::u32 passes{};
            if (!as_u32(reader.integer(schema::box_blur_radius, 0), radius) ||
                !as_u32(reader.integer(schema::box_blur_passes, 1), passes)) {
                reader.fail();
            }
            const auto normalized = normalized_box_blur(
                BoxBlurParameters{radius, passes});
            if (!normalized.has_value()) {
                reader.fail();
            } else {
                effect.box_blur = *normalized;
            }
        } else if (kind == schema::directional_blur_kind) {
            effect.kind = EffectKind::DirectionalBlur;
            const auto normalized = normalized_directional_blur(
                DirectionalBlurParameters{
                    reader.number(schema::directional_blur_length, 0.0F),
                    reader.number(schema::directional_blur_angle_turns,
                                  0.0F)});
            if (!normalized.has_value()) {
                reader.fail();
            } else {
                effect.directional_blur = *normalized;
            }
        } else if (kind == schema::chroma_key_kind) {
            effect.kind = EffectKind::ChromaKey;
            const auto parameters = read_chroma_key(reader);
            if (!parameters.has_value()) {
                reader.fail();
            } else {
                effect.chroma_key = *parameters;
            }
        } else if (kind == schema::luma_key_kind) {
            effect.kind = EffectKind::LumaKey;
            const auto parameters = read_luma_key(reader);
            if (!parameters.has_value()) {
                reader.fail();
            } else {
                effect.luma_key = *parameters;
            }
        } else if (kind == schema::levels_kind) {
            effect.kind = EffectKind::Levels;
            const auto levels = read_levels(reader);
            if (!levels.has_value()) {
                reader.fail();
            } else {
                effect.levels = *levels;
            }
        } else if (kind == schema::lift_gamma_gain_kind) {
            effect.kind = EffectKind::LiftGammaGain;
            const auto parameters = read_lift_gamma_gain(reader);
            if (!parameters.has_value()) {
                reader.fail();
            } else {
                effect.lift_gamma_gain = *parameters;
            }
        } else if (kind == schema::contrast_pivot_kind) {
            effect.kind = EffectKind::ContrastPivot;
            const auto parameters = read_contrast_pivot(reader);
            if (!parameters.has_value()) {
                reader.fail();
            } else {
                effect.contrast_pivot = *parameters;
            }
        } else if (kind == schema::color_matrix_kind) {
            effect.kind = EffectKind::ColorMatrix;
            const auto parameters = read_color_matrix(reader);
            if (!parameters.has_value()) {
                reader.fail();
            } else {
                effect.color_matrix = *parameters;
            }
        } else if (kind == schema::white_balance_kind) {
            effect.kind = EffectKind::WhiteBalance;
            const auto parameters = read_white_balance(reader);
            if (!parameters.has_value()) {
                reader.fail();
            } else {
                effect.white_balance = *parameters;
            }
        } else if (kind == schema::curves_kind) {
            effect.kind = EffectKind::Curves;
            const auto parameters = read_curves(reader, node.properties);
            if (!parameters.has_value()) {
                reader.fail();
            } else {
                effect.curves = *parameters;
            }
        } else if (kind == schema::film_grain_kind) {
            effect.kind = EffectKind::FilmGrain;
            const auto parameters = read_film_grain(reader);
            if (!parameters.has_value()) {
                reader.fail();
            } else {
                effect.film_grain = *parameters;
            }
        } else {
            reader.fail();
        }
        effect.order = reader.integer(schema::effect_order, 0);
        effect.enabled = reader.boolean(schema::effect_enabled, true);
        parsed = Entry{effect};
    } else if (node.type == schema::mask_type) {
        MaskEntry mask{};
        mask.scene = read_scene_id(reader, node.properties);
        mask.name = std::string{reader.text(schema::mask_name, "Mask")};
        if (mask.name.empty()
            || mask.name.size() > max_mask_name_bytes) {
            reader.fail();
        }
        const auto target = reader.integer(schema::mask_target, -1);
        if (target < 0) {
            reader.fail();
        } else {
            mask.target = doc::NodeId{static_cast<core::u64>(target)};
        }
        const std::string_view kind =
            reader.text(schema::mask_kind, schema::mask_rectangle_kind);
        if (kind == schema::mask_rectangle_kind) {
            mask.kind = MaskKind::Rectangle;
        } else if (kind == schema::mask_ellipse_kind) {
            mask.kind = MaskKind::Ellipse;
        } else if (kind == schema::mask_bezier_kind) {
            mask.kind = MaskKind::Bezier;
        } else {
            reader.fail();
        }
        const std::string_view combine =
            reader.text(schema::mask_combine, schema::mask_add);
        if (combine == schema::mask_add) {
            mask.combine = MaskCombine::Add;
        } else if (combine == schema::mask_subtract) {
            mask.combine = MaskCombine::Subtract;
        } else if (combine == schema::mask_intersect) {
            mask.combine = MaskCombine::Intersect;
        } else if (combine == schema::mask_difference) {
            mask.combine = MaskCombine::Difference;
        } else {
            reader.fail();
        }
        mask.order = reader.integer(schema::mask_order, 0);
        mask.enabled = reader.boolean(schema::mask_enabled, true);
        mask.feather_canvas_px = std::clamp(
            reader.number(schema::mask_feather_px, 0.0F), 0.0F,
            max_mask_feather_canvas_px);
        mask.expansion_canvas_px = std::clamp(
            reader.number(schema::mask_expansion_px, 0.0F),
            -max_mask_expansion_canvas_px,
            max_mask_expansion_canvas_px);
        mask.opacity = std::clamp(
            reader.number(schema::mask_opacity, 1.0F), 0.0F, 1.0F);
        mask.invert = reader.boolean(schema::mask_invert, false);
        mask.bounds.left = std::clamp(
            reader.number(schema::mask_left, 0.0F), 0.0F, 1.0F);
        mask.bounds.top = std::clamp(
            reader.number(schema::mask_top, 0.0F), 0.0F, 1.0F);
        mask.bounds.right = std::clamp(
            reader.number(schema::mask_right, 1.0F), 0.0F, 1.0F);
        mask.bounds.bottom = std::clamp(
            reader.number(schema::mask_bottom, 1.0F), 0.0F, 1.0F);
        mask.rotation_turns = reader.number(
            schema::mask_rotation_turns, 0.0F);
        if (std::isfinite(mask.rotation_turns)) {
            mask.rotation_turns -= std::floor(mask.rotation_turns + 0.5F);
        } else {
            reader.fail();
        }
        parsed = Entry{std::move(mask)};
    } else if (node.type == schema::mask_point_type) {
        MaskPointEntry point{};
        point.scene = read_scene_id(reader, node.properties);
        const auto parent = reader.integer(schema::mask_id, -1);
        if (parent < 0) {
            reader.fail();
        } else {
            point.mask = doc::NodeId{static_cast<core::u64>(parent)};
        }
        point.order = reader.integer(schema::mask_point_order, 0);
        point.x = std::clamp(
            reader.number(schema::mask_point_x, 0.0F), 0.0F, 1.0F);
        point.y = std::clamp(
            reader.number(schema::mask_point_y, 0.0F), 0.0F, 1.0F);
        point.in_x = std::clamp(
            reader.number(schema::mask_point_in_x, point.x), 0.0F, 1.0F);
        point.in_y = std::clamp(
            reader.number(schema::mask_point_in_y, point.y), 0.0F, 1.0F);
        point.out_x = std::clamp(
            reader.number(schema::mask_point_out_x, point.x), 0.0F, 1.0F);
        point.out_y = std::clamp(
            reader.number(schema::mask_point_out_y, point.y), 0.0F, 1.0F);
        parsed = Entry{point};
    } else if (node.type == schema::channel_type) {
        ChannelEntry channel{};
        const auto target = reader.integer(schema::channel_target, -1);
        if (target < 0) {
            reader.fail();
        }
        channel.target = doc::NodeId{static_cast<core::u64>(target)};

        // The component names are the transform property names, so authoring a
        // channel needs no vocabulary the operator doesn't already know.
        const auto component = reader.text(schema::channel_component, "");
        if (component == schema::position_x) {
            channel.component = TransformChannel::PositionX;
        } else if (component == schema::position_y) {
            channel.component = TransformChannel::PositionY;
        } else if (component == schema::position_z) {
            channel.component = TransformChannel::PositionZ;
        } else if (component == schema::rotation_yaw) {
            channel.component = TransformChannel::RotationYaw;
        } else if (component == schema::rotation_pitch) {
            channel.component = TransformChannel::RotationPitch;
        } else if (component == schema::rotation_roll) {
            channel.component = TransformChannel::RotationRoll;
        } else if (component == schema::scale_x) {
            channel.component = TransformChannel::ScaleX;
        } else if (component == schema::scale_y) {
            channel.component = TransformChannel::ScaleY;
        } else if (component == schema::scale_z) {
            channel.component = PropertyChannel::ScaleZ;
        } else if (component == schema::opacity) {
            channel.component = PropertyChannel::Opacity;
        } else if (component == schema::brightness) {
            channel.component = PropertyChannel::Brightness;
        } else if (component == schema::saturation) {
            channel.component = PropertyChannel::Saturation;
        } else if (component == schema::blur_radius) {
            channel.component = PropertyChannel::BlurRadius;
        } else if (component == schema::film_grain_amount) {
            channel.component = PropertyChannel::FilmGrainAmount;
        } else if (component == schema::box_blur_radius) {
            channel.component = PropertyChannel::BoxBlurRadius;
        } else if (component == schema::directional_blur_length) {
            channel.component = PropertyChannel::DirectionalBlurLength;
        } else if (component == schema::chroma_key_mix) {
            channel.component = PropertyChannel::ChromaKeyMix;
        } else if (component == schema::luma_key_mix) {
            channel.component = PropertyChannel::LumaKeyMix;
        } else if (component == schema::sky_time_of_day) {
            channel.component = PropertyChannel::SkyTimeOfDay;
        } else if (component == schema::environment_map_intensity) {
            channel.component = PropertyChannel::EnvironmentIntensity;
        } else if (component == schema::environment_map_rotation) {
            channel.component = PropertyChannel::EnvironmentRotation;
        } else if (component == schema::environment_fog_density) {
            channel.component = PropertyChannel::FogDensity;
        } else if (component == schema::environment_fog_height) {
            channel.component = PropertyChannel::FogHeight;
        } else if (component == schema::environment_fog_height_falloff) {
            channel.component = PropertyChannel::FogFalloff;
        } else if (component == schema::environment_fog_anisotropy) {
            channel.component = PropertyChannel::FogAnisotropy;
        } else if (component == schema::sky_haze) {
            channel.component = PropertyChannel::SkyHaze;
        } else if (component == schema::sky_solar_illuminance) {
            channel.component = PropertyChannel::SkyIlluminance;
        } else if (component == schema::intensity) {
            channel.component = PropertyChannel::LightIntensity;
        } else if (component == schema::color_r) {
            channel.component = PropertyChannel::LightColorR;
        } else if (component == schema::color_g) {
            channel.component = PropertyChannel::LightColorG;
        } else if (component == schema::color_b) {
            channel.component = PropertyChannel::LightColorB;
        } else if (component == schema::range) {
            channel.component = PropertyChannel::LightRange;
        } else if (component == schema::fov_turns) {
            channel.component = PropertyChannel::CameraFieldOfView;
        } else if (component == schema::shutter_seconds) {
            channel.component = PropertyChannel::CameraShutter;
        } else if (component == schema::focus_distance_m) {
            channel.component = PropertyChannel::CameraFocusDistance;
        } else if (component == schema::aperture_f_number) {
            channel.component = PropertyChannel::CameraAperture;
        } else if (std::string_view{component}.starts_with(schema::particle_parameter_prefix)
                   && parse_particle_parameter_key(std::string_view{component}.substr(
                          schema::particle_parameter_prefix.size()))) {
            channel.component = PropertyChannel::ParticleParameter;
            channel.parameter = std::string{
                std::string_view{component}.substr(schema::particle_parameter_prefix.size())};
        } else {
            reader.fail();
        }

        // Keys are indexed properties parsed in order until the first gap. A key
        // absent its frame ends the list; a key present but malformed rejects the
        // whole channel, like any other typed property.
        const std::string prefix{schema::channel_key_prefix};
        for (core::u32 i = 0U; i < max_channel_keys; ++i) {
            const auto index = std::to_string(i);
            const auto* const frame = find(
                node.properties,
                prefix + index + std::string{schema::channel_key_frame_suffix});
            if (frame == nullptr) {
                break;
            }
            ChannelKey key{};
            if (const auto* const whole = std::get_if<std::int64_t>(frame)) {
                key.frame = *whole;
            } else {
                reader.fail();
                break;
            }
            if (const auto* const value = find(
                    node.properties,
                    prefix + index
                        + std::string{schema::channel_key_value_suffix})) {
                if (const auto* const real = std::get_if<double>(value)) {
                    key.value = *real;
                } else if (const auto* const whole =
                               std::get_if<std::int64_t>(value)) {
                    key.value = static_cast<double>(*whole);
                } else {
                    reader.fail();
                }
            }
            constexpr double max_channel_value =
                static_cast<double>(std::numeric_limits<float>::max());
            if (!std::isfinite(key.value)
                || key.value < -max_channel_value
                || key.value > max_channel_value) {
                reader.fail();
            }
            if (const auto* const interp = find(
                    node.properties,
                    prefix + index
                        + std::string{schema::channel_key_interp_suffix})) {
                const auto* const name = std::get_if<std::string>(interp);
                if (name == nullptr) {
                    reader.fail();
                } else if (*name == schema::interp_hold) {
                    key.interp = ChannelInterp::Hold;
                } else if (*name == schema::interp_linear) {
                    key.interp = ChannelInterp::Linear;
                } else if (*name == schema::interp_cubic) {
                    key.interp = ChannelInterp::Cubic;
                } else {
                    reader.fail();
                }
            }

            const auto read_optional_number =
                [&node, &prefix, &index, &reader](
                    std::string_view suffix,
                    std::optional<double>& result) noexcept {
                    const auto* const held = find(
                        node.properties,
                        prefix + index + std::string{suffix});
                    if (held == nullptr) {
                        result.reset();
                        return;
                    }
                    double number{};
                    if (const auto* const real = std::get_if<double>(held)) {
                        number = *real;
                    } else if (const auto* const whole =
                                   std::get_if<std::int64_t>(held)) {
                        number = static_cast<double>(*whole);
                    } else {
                        reader.fail();
                        return;
                    }
                    if (!std::isfinite(number)) {
                        reader.fail();
                        return;
                    }
                    result = number;
                };
            if (const auto* const mode = find(
                    node.properties,
                    prefix + index
                        + std::string{
                            schema::channel_key_tangent_mode_suffix})) {
                const auto* const name = std::get_if<std::string>(mode);
                if (name == nullptr) {
                    reader.fail();
                } else if (*name == schema::tangent_auto) {
                    key.tangent_mode = KeyTangentMode::Auto;
                } else if (*name == schema::tangent_aligned) {
                    key.tangent_mode = KeyTangentMode::Aligned;
                } else if (*name == schema::tangent_broken) {
                    key.tangent_mode = KeyTangentMode::Broken;
                } else if (*name == schema::tangent_flat) {
                    key.tangent_mode = KeyTangentMode::Flat;
                } else {
                    reader.fail();
                }
            }
            std::optional<double> in_slope;
            std::optional<double> out_slope;
            read_optional_number(
                schema::channel_key_in_slope_suffix, in_slope);
            read_optional_number(
                schema::channel_key_out_slope_suffix, out_slope);
            read_optional_number(
                schema::channel_key_in_weight_suffix, key.in_weight);
            read_optional_number(
                schema::channel_key_out_weight_suffix, key.out_weight);
            if ((key.in_weight.has_value() && *key.in_weight <= 0.0)
                || (key.out_weight.has_value()
                    && *key.out_weight <= 0.0)) {
                reader.fail();
            }

            // Hold and Linear never retain dormant handles. Auto always
            // derives both slopes and one-third weights from its neighbours.
            // Flat keeps timing weights but canonicalizes both slopes to zero.
            if (key.interp != ChannelInterp::Cubic
                || key.tangent_mode == KeyTangentMode::Auto) {
                key.tangent_mode = KeyTangentMode::Auto;
                key.in_slope = 0.0;
                key.out_slope = 0.0;
                key.in_weight.reset();
                key.out_weight.reset();
            } else if (key.tangent_mode == KeyTangentMode::Flat) {
                key.in_slope = 0.0;
                key.out_slope = 0.0;
            } else if (key.tangent_mode == KeyTangentMode::Aligned) {
                const double aligned = in_slope.has_value()
                    ? (out_slope.has_value()
                           ? std::midpoint(*in_slope, *out_slope)
                           : *in_slope)
                    : out_slope.value_or(0.0);
                key.in_slope = aligned;
                key.out_slope = aligned;
            } else {
                key.in_slope = in_slope.value_or(0.0);
                key.out_slope = out_slope.value_or(0.0);
            }
            channel.keys.push_back(key);
        }
        if (channel.keys.empty()) {
            reader.fail();
        }
        // Sorted so evaluation brackets a frame by search; stable so equal-frame
        // keys keep authoring order, and two nodes order them identically.
        std::stable_sort(
            channel.keys.begin(),
            channel.keys.end(),
            [](const ChannelKey& left, const ChannelKey& right) noexcept {
                return left.frame < right.frame;
            });
        if (!valid_channel(channel.keys)) {
            reader.fail();
        }
        parsed = Entry{std::move(channel)};
    } else {
        // Not a scene node. Nothing to hold and nothing to report: the
        // document is full of nodes that are not this model's business.
        return;
    }

    if (!reader.ok()) {
        ++counters_.rejected;
        return;
    }

    note(*parsed, true);
    entries_.insert_or_assign(node.id, std::move(*parsed));
}

void SceneModel::reset(const doc::Root& root) {
    incoming_root_ = {}; accepted_root_ = {}; linked_ = {}; linked_error_.reset(); links_present_ = false;
    entries_.clear();
    wall_colors_.clear();
    history_nodes_.clear();
    document_version_ = doc::Version{};
    counters_ = SceneCounters{};
    loaded_ = true;
    // Diffing against an empty root enumerates every node in ascending id
    // order, which is the enumeration the document does not otherwise offer -
    // and adopt() is the same code a delta ends in, so a checkpointed node
    // and a streaming one cannot parse differently. The document itself is
    // taken as it stands: it is immutable and shared, so holding it copies
    // nothing.
    const bool links_dirty = touches_links(diff(doc::Root{}, root));
    incoming_root_ = root;
    adopt(root.version(), links_dirty);
}

void SceneModel::update(const doc::Root& root) {
    // A model holding a refused document is in the state a rebuild left it
    // in, with nothing accepted to move forward from.
    if (!loaded_ || linked_error_.has_value()) {
        reset(root);
        return;
    }
    const bool links_dirty = touches_links(diff(incoming_root_, root));
    incoming_root_ = root;
    adopt(root.version(), links_dirty);
    if (linked_error_.has_value()) {
        reset(root);
    }
}

bool SceneModel::touches_links(const doc::Delta& delta) noexcept {
    bool links_dirty = false;
    for (const auto& change : delta.changes) {
        if ((change.before && (is_link_record(*change.before) || change.before->properties.contains(instance_batch_property)))
            || (change.after && (is_link_record(*change.after) || change.after->properties.contains(instance_batch_property)))) {
            links_dirty = true; links_present_ = true;
        }
        // Everything else the link records are read against. Stages and
        // channels are named by them; models and lights are counted with them
        // against the scene's budgets, and a document past a budget is
        // refused whole. So with link records present, a change to any of
        // these has them read again - or a machine that followed the edits
        // would keep drawing a document one that loaded it cold refuses.
        const auto read_with_links = [](const doc::NodeRef& node) noexcept {
            return node && (node->type == schema::stage_type || node->type == schema::channel_type
                || node->type == schema::model_type || node->type == schema::light_type);
        };
        if (links_present_ && (read_with_links(change.before) || read_with_links(change.after))) links_dirty = true;
    }
    return links_dirty;
}

void SceneModel::apply(const doc::Delta& delta) {
    loaded_ = true;
    const bool links_dirty = touches_links(delta) || linked_error_.has_value();
    for (const auto& change : delta.changes) {
        if (change.after) incoming_root_ = incoming_root_.set(*change.after);
        else if (auto erased = incoming_root_.erase(change.id)) incoming_root_ = std::move(*erased);
    }
    adopt(delta.to, links_dirty);
}

void SceneModel::adopt(doc::Version to, bool links_dirty) {
    // Whole-candidate admission keeps the last valid rendering AND digest on a
    // malformed hierarchy. Track incoming state separately so a later repair
    // can adopt all changes since the last accepted revision.
    //
    // A node that stops being a linked occurrence is parsed differently from
    // then on, though the node itself did not change, so the occurrences
    // being replaced are remembered and parsed again below with the new ones.
    std::vector<doc::NodeId> former_occurrences;
    if (links_dirty) {
        auto parsed_links = read_linked_scene(incoming_root_);
        if (!parsed_links) { linked_error_ = parsed_links.error(); ++counters_.rejected; return; }
        former_occurrences.reserve(linked_.occurrences.size());
        for (const auto& [id, occurrence] : linked_.occurrences) {
            static_cast<void>(occurrence);
            former_occurrences.push_back(id);
        }
        linked_ = std::move(parsed_links).value();
    }
    linked_error_.reset();
    const auto accepted_delta = doc::diff(accepted_root_, incoming_root_);
    accepted_root_ = incoming_root_;
    // A content digest survives both incremental adoption and full model
    // rebuilds. Hash typed properties in node-id/key order, excluding only
    // transport controls; actual authored edits still invalidate history.
    const auto word = [](core::u64& hash, core::u64 value) {
        for (unsigned byte = 0; byte < 8U; ++byte) {
            hash = (hash ^ (value & 0xffU)) * 1099511628211ULL;
            value >>= 8U;
        }
    };
    const auto text = [&word](core::u64& hash, std::string_view value) {
        word(hash, value.size());
        for (const unsigned char byte : value)
            hash = (hash ^ byte) * 1099511628211ULL;
    };
    for (const auto& change : accepted_delta.changes) {
        if (change.kind == doc::ChangeKind::Removed || !change.after
            || change.after->type == "show.transport") {
            history_nodes_.erase(change.id);
        } else {
            core::u64 hash = 14695981039346656037ULL;
            text(hash, change.after->type);
            word(hash, change.after->properties.size());
            for (const auto& [name, value] : change.after->properties) {
                text(hash, name);
                word(hash, value.index());
                if (const auto* boolean = std::get_if<bool>(&value)) word(hash, *boolean);
                else if (const auto* integer = std::get_if<std::int64_t>(&value)) word(hash, static_cast<core::u64>(*integer));
                else if (const auto* scalar = std::get_if<double>(&value)) word(hash, std::bit_cast<core::u64>(*scalar));
                else text(hash, std::get<std::string>(value));
            }
            history_nodes_.insert_or_assign(change.id, hash);
        }
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
    // Source edits affect occurrences even when no shot node changed.
    if (links_dirty) for (const auto& [id, occurrence] : linked_.occurrences) {
        static_cast<void>(occurrence);
        if (const auto* node = accepted_root_.get(id)) put(*node);
    }
    for (const doc::NodeId id : former_occurrences) {
        if (linked_.occurrences.contains(id)) continue;
        if (const auto* node = accepted_root_.get(id)) put(*node);
    }
    document_version_ = to;
    temporal_document_digest_ = 14695981039346656037ULL;
    for (const auto& [id, hash] : history_nodes_) {
        word(temporal_document_digest_, id.value());
        word(temporal_document_digest_, hash);
    }
    refresh_duplicate_cameras();
}

SceneId SceneModel::default_scene_id() const noexcept {
    for (const auto& [id, entry] : entries_) {
        if (std::holds_alternative<StageEntry>(entry)) {
            return id;
        }
    }
    return SceneId{};
}

bool SceneModel::valid_scene(SceneId scene) const noexcept {
    const auto found = entries_.find(scene);
    return found != entries_.end()
        && std::holds_alternative<StageEntry>(found->second);
}

bool SceneModel::effect_target(doc::NodeId id) const noexcept {
    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        return false;
    }
    // A track is here because a track is a screen: a grade on the wall is the
    // whole reason an operator makes one, and it applies to every clip that
    // plays on it rather than to a picture the track draws itself.
    return std::holds_alternative<ObjectEntry>(found->second)
        || std::holds_alternative<MediaEntry>(found->second)
        || std::holds_alternative<ModelEntry>(found->second)
        || std::holds_alternative<LaneEntry>(found->second);
}

std::optional<SceneId> SceneModel::scope_of(doc::NodeId id) const noexcept {
    const auto direct_scope = [](const Entry& entry)
        -> std::optional<SceneId> {
        if (const auto* value = std::get_if<CameraEntry>(&entry)) {
            return value->scene;
        }
        if (const auto* value = std::get_if<ObjectEntry>(&entry)) {
            return value->scene;
        }
        if (const auto* value = std::get_if<GroupEntry>(&entry)) {
            return value->scene;
        }
        if (const auto* value = std::get_if<LaneEntry>(&entry)) {
            return value->scene;
        }
        if (const auto* value = std::get_if<MarkerEntry>(&entry)) {
            return value->scene;
        }
        if (const auto* value = std::get_if<MediaEntry>(&entry)) {
            return value->scene;
        }
        if (const auto* value = std::get_if<ModelEntry>(&entry)) {
            return value->scene;
        }
        if (const auto* value = std::get_if<ParticleEffectEntry>(&entry)) {
            return value->scene;
        }
        if (const auto* value = std::get_if<LightEntry>(&entry)) {
            return value->scene;
        }
        if (const auto* value = std::get_if<MaskEntry>(&entry)) {
            return value->scene;
        }
        return std::nullopt;
    };

    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    if (const auto scope = direct_scope(found->second)) {
        return scope;
    }
    if (std::holds_alternative<StageEntry>(found->second)) {
        return id;
    }
    if (const auto* effect = std::get_if<EffectEntry>(&found->second)) {
        if (!effect_target(effect->target)) {
            return std::nullopt;
        }
        const auto target = entries_.find(effect->target);
        return direct_scope(target->second);
    }
    if (const auto* channel = std::get_if<ChannelEntry>(&found->second)) {
        const auto target = entries_.find(channel->target);
        if (target == entries_.end()) {
            return std::nullopt;
        }
        if (std::holds_alternative<StageEntry>(target->second)) return channel->target;
        if (const auto scope = direct_scope(target->second)) {
            return scope;
        }
        if (const auto* effect = std::get_if<EffectEntry>(&target->second)) {
            if (!effect_target(effect->target)) {
                return std::nullopt;
            }
            const auto drawable_target = entries_.find(effect->target);
            return direct_scope(drawable_target->second);
        }
    }
    if (const auto* point = std::get_if<MaskPointEntry>(&found->second)) {
        return point->scene;
    }
    return std::nullopt;
}

SceneModel::GroupProjection SceneModel::resolve_groups(
    SceneId scene_id,
    const std::map<doc::NodeId, GroupLocal>* local_overrides) const {
    GroupProjection projection{};
    if (!valid_scene(scene_id)) {
        return projection;
    }

    enum class Visit : core::u8 { Unknown, Visiting, Valid, Invalid };
    std::map<doc::NodeId, Visit> visits;
    std::vector<doc::NodeId> stack;

    const auto group_at = [this](doc::NodeId id) -> const GroupEntry* {
        const auto found = entries_.find(id);
        return found == entries_.end()
            ? nullptr
            : std::get_if<GroupEntry>(&found->second);
    };
    const auto in_scene = [scene_id](
                              const GroupEntry& group) noexcept {
        return group.scene == scene_id;
    };
    const auto diagnose = [&projection](GroupDiagnosticKind kind,
                                        doc::NodeId subject,
                                        doc::NodeId related) {
        projection.diagnostics.push_back(
            GroupDiagnostic{kind, subject, related});
    };

    std::function<bool(doc::NodeId)> validate;
    validate = [&](doc::NodeId id) -> bool {
        Visit& visit = visits[id];
        if (visit == Visit::Valid) {
            return true;
        }
        if (visit == Visit::Invalid) {
            return false;
        }
        if (visit == Visit::Visiting) {
            const auto cycle = std::find(stack.begin(), stack.end(), id);
            for (auto current = cycle; current != stack.end(); ++current) {
                visits[*current] = Visit::Invalid;
                const GroupEntry* const group = group_at(*current);
                diagnose(GroupDiagnosticKind::Cycle, *current,
                         group != nullptr && group->parent_group.has_value()
                             ? *group->parent_group
                             : *current);
            }
            return false;
        }

        const GroupEntry* const group = group_at(id);
        if (group == nullptr || !in_scene(*group)) {
            visit = Visit::Invalid;
            return false;
        }
        visit = Visit::Visiting;
        stack.push_back(id);
        if (group->parent_group.has_value()) {
            const doc::NodeId parent = *group->parent_group;
            const GroupEntry* const parent_group = group_at(parent);
            if (parent_group == nullptr) {
                diagnose(GroupDiagnosticKind::MissingParent, id, parent);
                visit = Visit::Invalid;
            } else if (!in_scene(*parent_group)) {
                diagnose(GroupDiagnosticKind::CrossSceneParent, id, parent);
                visit = Visit::Invalid;
            } else if (!validate(parent) && visit != Visit::Invalid) {
                diagnose(GroupDiagnosticKind::InvalidAncestor, id, parent);
                visit = Visit::Invalid;
            }
        }
        stack.pop_back();
        if (visit == Visit::Visiting) {
            visit = Visit::Valid;
        }
        return visit == Visit::Valid;
    };

    for (const auto& [id, entry] : entries_) {
        const auto* const group = std::get_if<GroupEntry>(&entry);
        if (group != nullptr && in_scene(*group)) {
            static_cast<void>(validate(id));
        }
    }

    std::function<void(doc::NodeId)> compose;
    compose = [&](doc::NodeId id) {
        if (projection.resolved.contains(id) || visits[id] != Visit::Valid) {
            return;
        }
        const GroupEntry& group = *group_at(id);
        if (group.parent_group.has_value()) {
            compose(*group.parent_group);
        }
        GroupLocal local{group.position, group.yaw, group.pitch, group.roll,
                         group.scale};
        if (local_overrides != nullptr) {
            const auto found = local_overrides->find(id);
            if (found != local_overrides->end()) {
                local = found->second;
            }
        }
        const Mat4 local_matrix = multiply(
            translation(local.position),
            multiply(rotation(local.yaw, local.pitch, local.roll),
                     scaling(local.scale)));
        GroupResolution resolved{local_matrix, group.locked, group.visible};
        if (group.parent_group.has_value()) {
            const GroupResolution& parent =
                projection.resolved.at(*group.parent_group);
            resolved.world = multiply(parent.world, local_matrix);
            resolved.locked = parent.locked || group.locked;
            resolved.visible = parent.visible && group.visible;
        }
        projection.resolved.emplace(id, resolved);
    };
    for (const auto& [id, visit] : visits) {
        if (visit == Visit::Valid) {
            compose(id);
        }
    }

    // Report invalid parent edges on drawables as well as on groups. They are
    // omitted by evaluate(), but remain ordinary parsed nodes so the editor
    // can repair the exact bad reference.
    const auto diagnose_drawable = [&](doc::NodeId id,
                                       const auto& drawable) {
        if (!drawable.parent_group.has_value()
            || drawable.scene != scene_id) {
            return;
        }
        const doc::NodeId parent = *drawable.parent_group;
        const GroupEntry* const group = group_at(parent);
        if (group == nullptr) {
            diagnose(GroupDiagnosticKind::MissingParent, id, parent);
        } else if (!in_scene(*group)) {
            diagnose(GroupDiagnosticKind::CrossSceneParent, id, parent);
        } else if (!projection.resolved.contains(parent)) {
            diagnose(GroupDiagnosticKind::InvalidAncestor, id, parent);
        }
    };
    for (const auto& [id, entry] : entries_) {
        if (const auto* object = std::get_if<ObjectEntry>(&entry)) {
            diagnose_drawable(id, *object);
        } else if (const auto* media = std::get_if<MediaEntry>(&entry)) {
            diagnose_drawable(id, *media);
        } else if (const auto* model = std::get_if<ModelEntry>(&entry)) {
            diagnose_drawable(id, *model);
        } else if (const auto* effect = std::get_if<ParticleEffectEntry>(&entry)) {
            diagnose_drawable(id, *effect);
        }
    }

    std::stable_sort(
        projection.diagnostics.begin(), projection.diagnostics.end(),
        [](const GroupDiagnostic& left,
           const GroupDiagnostic& right) noexcept {
            return std::tuple{left.subject.value(),
                              static_cast<core::u8>(left.kind),
                              left.related.value()}
                < std::tuple{right.subject.value(),
                             static_cast<core::u8>(right.kind),
                             right.related.value()};
        });
    projection.diagnostics.erase(
        std::unique(projection.diagnostics.begin(),
                    projection.diagnostics.end()),
        projection.diagnostics.end());

    // Parent-first preorder with explicit sibling order and node id as the
    // final tie-break. This makes hierarchy rows stable under streamed deltas.
    std::map<std::optional<doc::NodeId>, std::vector<doc::NodeId>> children;
    for (const auto& [id, resolution] : projection.resolved) {
        static_cast<void>(resolution);
        children[group_at(id)->parent_group].push_back(id);
    }
    for (auto& [parent, siblings] : children) {
        static_cast<void>(parent);
        std::stable_sort(
            siblings.begin(), siblings.end(),
            [&group_at](doc::NodeId left, doc::NodeId right) noexcept {
                const GroupEntry& left_group = *group_at(left);
                const GroupEntry& right_group = *group_at(right);
                return std::pair{left_group.group_order, left.value()}
                    < std::pair{right_group.group_order, right.value()};
            });
    }
    std::function<void(doc::NodeId)> append;
    append = [&](doc::NodeId id) {
        if (projection.groups.size() >= max_scene_groups) {
            return;
        }
        const GroupEntry& group = *group_at(id);
        GroupLocal local{group.position, group.yaw, group.pitch, group.roll,
                         group.scale};
        if (local_overrides != nullptr) {
            const auto found = local_overrides->find(id);
            if (found != local_overrides->end()) {
                local = found->second;
            }
        }
        const GroupResolution& resolved = projection.resolved.at(id);
        projection.groups.push_back(SceneGroup{
            id, scene_id, group.name, group.parent_group, group.group_order,
            local.position, local.yaw, local.pitch, local.roll, local.scale,
            resolved.world, group.locked, group.visible, resolved.locked,
            resolved.visible, group.kind});
        const auto descendants = children.find(id);
        if (descendants != children.end()) {
            for (const doc::NodeId child : descendants->second) {
                append(child);
            }
        }
    };
    const auto roots = children.find(std::nullopt);
    if (roots != children.end()) {
        for (const doc::NodeId id : roots->second) {
            append(id);
        }
    }
    return projection;
}

SceneModel::MaskProjection SceneModel::resolve_masks(
    SceneId scene_id, const GroupProjection& groups) const {
    MaskProjection projection{};
    if (!valid_scene(scene_id)) {
        return projection;
    }
    const SceneId default_scene = default_scene_id();
    const auto diagnose = [&projection](MaskDiagnosticKind kind,
                                        doc::NodeId subject,
                                        doc::NodeId related) {
        projection.diagnostics.push_back(
            MaskDiagnostic{kind, subject, related});
    };
    const auto authored_scene = [this](SceneId id) noexcept {
        const auto found = entries_.find(id);
        return found != entries_.end()
            && std::holds_alternative<StageEntry>(found->second);
    };

    std::map<doc::NodeId, const MaskEntry*> masks_by_id;
    for (const auto& [id, entry] : entries_) {
        if (const auto* const mask = std::get_if<MaskEntry>(&entry)) {
            masks_by_id.emplace(id, mask);
        }
    }

    // Child nodes are first-class and can arrive before their mask in a delta.
    // New mask nodes deliberately have no implicit/default ownership adoption:
    // both the point and its parent require an explicit authored stage id, and
    // those ids must match. Invalid/unowned records route to the lowest stage (or
    // the stage-less diagnostic scene) only for presentation, never evaluation.
    std::map<doc::NodeId, std::vector<MaskPoint>> points_by_mask;
    std::map<doc::NodeId, bool> point_limit_invalid;
    core::u32 points_seen{};
    for (const auto& [id, entry] : entries_) {
        const auto* const point = std::get_if<MaskPointEntry>(&entry);
        if (point == nullptr) {
            continue;
        }
        const auto parent = masks_by_id.find(point->mask);
        const MaskEntry* const mask = parent == masks_by_id.end()
            ? nullptr
            : parent->second;
        SceneId diagnostic_scene = default_scene;
        if (point->scene.has_value() && authored_scene(*point->scene)) {
            diagnostic_scene = *point->scene;
        } else if (mask != nullptr && mask->scene.has_value()
                   && authored_scene(*mask->scene)) {
            diagnostic_scene = *mask->scene;
        }
        if (diagnostic_scene != scene_id) {
            continue;
        }
        if (!point->scene.has_value()) {
            diagnose(MaskDiagnosticKind::MissingSceneOwner, id,
                     point->mask);
            continue;
        }
        if (!authored_scene(*point->scene)) {
            diagnose(MaskDiagnosticKind::InvalidSceneOwner, id,
                     *point->scene);
            continue;
        }
        const SceneId point_scene = *point->scene;
        if (points_seen >= max_scene_mask_points) {
            diagnose(MaskDiagnosticKind::PointLimitExceeded, id,
                     point->mask);
            if (mask != nullptr && mask->scene == point->scene) {
                point_limit_invalid[point->mask] = true;
            }
            continue;
        }
        ++points_seen;
        if (mask == nullptr) {
            diagnose(MaskDiagnosticKind::MissingMask, id, point->mask);
            continue;
        }
        if (!mask->scene.has_value() || *mask->scene != point_scene) {
            diagnose(MaskDiagnosticKind::CrossSceneMask, id, point->mask);
            continue;
        }
        points_by_mask[point->mask].push_back(MaskPoint{
            id, scene_id, point->mask, point->order, point->x, point->y,
            point->in_x, point->in_y, point->out_x, point->out_y});
    }

    std::map<doc::NodeId, bool> path_valid;
    std::map<doc::NodeId, bool> over_limit_active_targets;
    core::u32 masks_seen{};
    for (const auto& [id, entry] : entries_) {
        const auto* const mask = std::get_if<MaskEntry>(&entry);
        if (mask == nullptr) {
            continue;
        }
        if (!mask->scene.has_value()) {
            if (scene_id == default_scene) {
                diagnose(MaskDiagnosticKind::MissingSceneOwner, id,
                         mask->target);
            }
            continue;
        }
        if (!authored_scene(*mask->scene)) {
            if (scene_id == default_scene) {
                diagnose(MaskDiagnosticKind::InvalidSceneOwner, id,
                         *mask->scene);
            }
            continue;
        }
        if (*mask->scene != scene_id) {
            continue;
        }
        if (masks_seen >= max_scene_masks) {
            diagnose(MaskDiagnosticKind::MaskLimitExceeded, id,
                     mask->target);
            if (mask->enabled) {
                over_limit_active_targets[mask->target] = true;
            }
            continue;
        }
        ++masks_seen;

        MaskInstance instance{};
        instance.source = id;
        instance.scene = scene_id;
        instance.name = mask->name;
        instance.target = mask->target;
        instance.kind = mask->kind;
        instance.combine = mask->combine;
        instance.order = mask->order;
        instance.enabled = mask->enabled;
        instance.feather_canvas_px = mask->feather_canvas_px;
        instance.expansion_canvas_px = mask->expansion_canvas_px;
        instance.opacity = mask->opacity;
        instance.invert = mask->invert;
        instance.bounds = mask->bounds;
        instance.rotation_turns = mask->rotation_turns;
        if (const auto found = points_by_mask.find(id);
            found != points_by_mask.end()) {
            instance.points = found->second;
        }
        std::stable_sort(
            instance.points.begin(), instance.points.end(),
            [](const MaskPoint& left, const MaskPoint& right) noexcept {
                return std::pair{left.order, left.source.value()}
                    < std::pair{right.order, right.source.value()};
            });

        bool valid_path = !point_limit_invalid[id];
        if (instance.points.size() > max_mask_points_per_mask) {
            diagnose(MaskDiagnosticKind::PointLimitExceeded,
                     instance.points[max_mask_points_per_mask].source, id);
            instance.points.resize(max_mask_points_per_mask);
            valid_path = false;
        }
        for (std::size_t index = 1U; index < instance.points.size(); ++index) {
            if (instance.points[index - 1U].order
                == instance.points[index].order) {
                diagnose(MaskDiagnosticKind::DuplicatePointOrder,
                         instance.points[index].source,
                         instance.points[index - 1U].source);
                valid_path = false;
            }
        }
        if (instance.kind == MaskKind::Bezier) {
            if (instance.points.size() < 3U) {
                diagnose(MaskDiagnosticKind::MalformedPath, id,
                         instance.target);
                valid_path = false;
            }
        } else {
            if (!instance.points.empty()) {
                diagnose(MaskDiagnosticKind::MalformedPath, id,
                         instance.points.front().source);
                valid_path = false;
            }
            if (!(instance.bounds.left < instance.bounds.right)
                || !(instance.bounds.top < instance.bounds.bottom)) {
                diagnose(MaskDiagnosticKind::MalformedPath, id,
                         instance.target);
                valid_path = false;
            }
        }
        path_valid[id] = valid_path;
        projection.masks.push_back(std::move(instance));
    }
    std::stable_sort(
        projection.masks.begin(), projection.masks.end(),
        [](const MaskInstance& left,
           const MaskInstance& right) noexcept {
            return std::tuple{left.target.value(), left.order,
                              left.source.value()}
                < std::tuple{right.target.value(), right.order,
                             right.source.value()};
        });

    const auto target_space = [this, scene_id, &groups,
                               &diagnose](const MaskInstance& mask)
        -> std::optional<MaskCoordinateSpace> {
        const auto found = entries_.find(mask.target);
        if (found == entries_.end()) {
            diagnose(MaskDiagnosticKind::MissingTarget, mask.source,
                     mask.target);
            return std::nullopt;
        }
        SceneId owner{};
        bool is_group{};
        if (const auto* object =
                std::get_if<ObjectEntry>(&found->second)) {
            owner = object->scene;
        } else if (const auto* media =
                       std::get_if<MediaEntry>(&found->second)) {
            owner = media->scene;
        } else if (const auto* model =
                       std::get_if<ModelEntry>(&found->second)) {
            owner = model->scene;
        } else if (const auto* effect =
                       std::get_if<ParticleEffectEntry>(&found->second)) {
            owner = effect->scene;
        } else if (const auto* group =
                       std::get_if<GroupEntry>(&found->second)) {
            owner = group->scene;
            is_group = true;
        } else {
            diagnose(MaskDiagnosticKind::InvalidTarget, mask.source,
                     mask.target);
            return std::nullopt;
        }
        if (owner != scene_id) {
            diagnose(MaskDiagnosticKind::CrossSceneTarget, mask.source,
                     mask.target);
            return std::nullopt;
        }
        if (is_group && !groups.resolved.contains(mask.target)) {
            diagnose(MaskDiagnosticKind::InvalidTarget, mask.source,
                     mask.target);
            return std::nullopt;
        }
        return is_group ? MaskCoordinateSpace::AuthoredCanvas
                        : MaskCoordinateSpace::SourceLayer;
    };

    struct ProgramBuild final {
        MaskCoordinateSpace space{MaskCoordinateSpace::SourceLayer};
        bool invalid{};
        std::vector<MaskInstance> active;
    };
    std::map<doc::NodeId, ProgramBuild> builds;
    for (const MaskInstance& mask : projection.masks) {
        const auto space = target_space(mask);
        if (!space.has_value()) {
            continue;
        }
        ProgramBuild& build = builds[mask.target];
        build.space = *space;
        if (!mask.enabled) {
            continue;
        }
        build.invalid = build.invalid || !path_valid[mask.source];
        build.active.push_back(mask);
    }
    for (auto& [target, build] : builds) {
        build.invalid = build.invalid || over_limit_active_targets[target];
        if (build.active.empty()) {
            continue;
        }
        if (build.active.front().combine != MaskCombine::Add) {
            diagnose(MaskDiagnosticKind::InvalidFirstCombine,
                     build.active.front().source, target);
            build.invalid = true;
        }
        if (!build.invalid) {
            projection.programs.push_back(MaskProgram{
                target, build.space, std::move(build.active)});
        }
    }

    std::stable_sort(
        projection.diagnostics.begin(), projection.diagnostics.end(),
        [](const MaskDiagnostic& left,
           const MaskDiagnostic& right) noexcept {
            return std::tuple{left.subject.value(),
                              static_cast<core::u8>(left.kind),
                              left.related.value()}
                < std::tuple{right.subject.value(),
                             static_cast<core::u8>(right.kind),
                             right.related.value()};
        });
    projection.diagnostics.erase(
        std::unique(projection.diagnostics.begin(),
                    projection.diagnostics.end()),
        projection.diagnostics.end());
    return projection;
}

void SceneModel::refresh_duplicate_cameras() noexcept {
    counters_.duplicate_cameras = 0U;
    std::map<SceneId, core::u32> per_scene;
    for (const auto& [id, entry] : entries_) {
        if (!std::holds_alternative<CameraEntry>(entry)) {
            continue;
        }
        const auto scene = scope_of(id);
        if (!scene.has_value() || !valid_scene(*scene)) {
            continue;
        }
        auto& count = per_scene[*scene];
        if (count != 0U) {
            ++counters_.duplicate_cameras;
        }
        ++count;
    }
}

std::vector<SceneInfo> SceneModel::scenes() const {
    std::vector<SceneInfo> result;
    result.reserve(counters_.stages);
    for (const auto& [id, entry] : entries_) {
        if (const auto* stage = std::get_if<StageEntry>(&entry)) {
            result.push_back(SceneInfo{id, stage->name, stage->canvas});
        }
    }
    return result;
}

double SceneModel::auto_tangent(std::span<const ChannelKey> channel,
                                std::size_t key_index) noexcept {
    const auto keys = channel;
    if (keys.size() <= 1U) {
        return 0.0;
    }
    const auto width = [&keys](std::size_t left) noexcept {
        return static_cast<double>(
            frame_distance(keys[left].frame, keys[left + 1U].frame));
    };
    const auto secant = [&keys, &width](std::size_t left) noexcept {
        return (keys[left + 1U].value - keys[left].value) / width(left);
    };
    if (keys.size() == 2U) {
        return secant(0U);
    }

    const auto endpoint = [](double first_width, double second_width,
                             double first_secant,
                             double second_secant) noexcept {
        double slope = ((2.0 * first_width + second_width) * first_secant
                        - first_width * second_secant)
            / (first_width + second_width);
        if (!same_direction(slope, first_secant)) {
            slope = 0.0;
        } else if (!same_direction(first_secant, second_secant)
                   && std::abs(slope)
                       > 3.0 * std::abs(first_secant)) {
            slope = 3.0 * first_secant;
        }
        return slope;
    };
    if (key_index == 0U) {
        return endpoint(width(0U), width(1U), secant(0U), secant(1U));
    }
    if (key_index + 1U == keys.size()) {
        const std::size_t last = keys.size() - 2U;
        return endpoint(
            width(last), width(last - 1U), secant(last), secant(last - 1U));
    }

    const double before = secant(key_index - 1U);
    const double after = secant(key_index);
    if (!same_direction(before, after)) {
        return 0.0;
    }
    const double before_width = width(key_index - 1U);
    const double after_width = width(key_index);
    const double first_weight = 2.0 * after_width + before_width;
    const double second_weight = after_width + 2.0 * before_width;
    const double denominator =
        first_weight / before + second_weight / after;
    if (!std::isfinite(denominator) || denominator == 0.0) {
        return 0.0;
    }
    return (first_weight + second_weight) / denominator;
}

double SceneModel::tangent_slope(std::span<const ChannelKey> channel,
                                 std::size_t key_index,
                                 bool incoming) noexcept {
    const ChannelKey& key = channel[key_index];
    if (key.interp != ChannelInterp::Cubic
        || key.tangent_mode == KeyTangentMode::Auto) {
        return auto_tangent(channel, key_index);
    }
    switch (key.tangent_mode) {
    case KeyTangentMode::Auto:
        return auto_tangent(channel, key_index);
    case KeyTangentMode::Aligned:
    case KeyTangentMode::Broken:
        return incoming ? key.in_slope : key.out_slope;
    case KeyTangentMode::Flat:
        return 0.0;
    }
    return 0.0;
}

bool SceneModel::valid_channel(std::span<const ChannelKey> channel) noexcept {
    if (channel.empty()) {
        return false;
    }
    constexpr core::u64 maximum_span =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    constexpr double maximum_value =
        static_cast<double>(std::numeric_limits<float>::max());
    for (const ChannelKey& key : channel) {
        if (!std::isfinite(key.value) || !std::isfinite(key.in_slope)
            || !std::isfinite(key.out_slope)
            || (key.in_weight.has_value()
                && (!std::isfinite(*key.in_weight)
                    || *key.in_weight <= 0.0))
            || (key.out_weight.has_value()
                && (!std::isfinite(*key.out_weight)
                    || *key.out_weight <= 0.0))) {
            return false;
        }
    }
    for (std::size_t index = 0U; index + 1U < channel.size(); ++index) {
        const ChannelKey& left = channel[index];
        const ChannelKey& right = channel[index + 1U];
        if (right.frame <= left.frame) {
            return false;
        }
        const core::u64 integral_span =
            frame_distance(left.frame, right.frame);
        if (integral_span > maximum_span) {
            return false;
        }
        if (left.interp != ChannelInterp::Cubic) {
            continue;
        }
        const double span = static_cast<double>(integral_span);
        const bool left_authored =
            left.tangent_mode != KeyTangentMode::Auto
            && left.out_weight.has_value();
        const bool right_authored =
            right.interp == ChannelInterp::Cubic
            && right.tangent_mode != KeyTangentMode::Auto
            && right.in_weight.has_value();
        const double out_weight =
            left_authored ? *left.out_weight : span / 3.0;
        const double in_weight =
            right_authored ? *right.in_weight : span / 3.0;
        if (!std::isfinite(out_weight + in_weight)
            || out_weight + in_weight > span) {
            return false;
        }
        const double first_handle = left.value
            + tangent_slope(channel, index, false) * out_weight;
        const double second_handle = right.value
            - tangent_slope(channel, index + 1U, true) * in_weight;
        if (!std::isfinite(first_handle) || !std::isfinite(second_handle)
            || first_handle < -maximum_value
            || first_handle > maximum_value
            || second_handle < -maximum_value
            || second_handle > maximum_value) {
            return false;
        }
    }
    return true;
}

double SceneModel::sample_channel(
    std::span<const ChannelKey> channel, core::i64 frame) noexcept {
    return sample_channel(channel, frame, 0.0);
}

double SceneModel::sample_channel(
    std::span<const ChannelKey> channel, core::i64 frame, double fraction) noexcept {
    const auto keys = channel;
    // Held flat outside the authored range, so a channel never extrapolates.
    // Keys sit on whole frames, so `frame + fraction` lies in the interval
    // that starts at or before `frame`.
    if (frame < keys.front().frame || (frame == keys.front().frame && fraction <= 0.0)) {
        return keys.front().value;
    }
    if (frame >= keys.back().frame) {
        return keys.back().value;
    }
    // The first key strictly after the frame; the one before it opens the
    // interval. Both exist: the clamps above rule out the ends.
    const auto after = std::upper_bound(
        keys.begin(),
        keys.end(),
        frame,
        [](core::i64 f, const ChannelKey& key) noexcept {
            return f < key.frame;
        });
    const auto& lo = *(after - 1);
    const auto& hi = *after;
    if (frame == lo.frame && fraction <= 0.0) {
        return lo.value;
    }
    if (lo.interp == ChannelInterp::Hold) {
        return lo.value;
    }
    const auto hi_index = static_cast<std::size_t>(after - keys.begin());
    const std::size_t lo_index = hi_index - 1U;
    const double span = static_cast<double>(
        frame_distance(lo.frame, hi.frame));
    const double offset = static_cast<double>(
        frame_distance(lo.frame, frame)) + fraction;
    if (lo.interp == ChannelInterp::Linear) {
        const double t = offset / span;
        return lo.value + (hi.value - lo.value) * t;
    }

    const bool out_weight_authored =
        lo.tangent_mode != KeyTangentMode::Auto
        && lo.out_weight.has_value();
    const bool in_weight_authored =
        hi.interp == ChannelInterp::Cubic
        && hi.tangent_mode != KeyTangentMode::Auto
        && hi.in_weight.has_value();
    const double out_weight =
        out_weight_authored ? *lo.out_weight : span / 3.0;
    const double in_weight =
        in_weight_authored ? *hi.in_weight : span / 3.0;

    double t{};
    if (!out_weight_authored && !in_weight_authored) {
        // One-third handles make x(t) exactly the linear interval parameter.
        t = offset / span;
    } else {
        const double first_x = out_weight;
        const double second_x = span - in_weight;
        double lower{};
        double upper{1.0};
        for (core::u32 iteration = 0U;
             iteration < key_bezier_bisection_iterations; ++iteration) {
            const double middle = (lower + upper) * 0.5;
            const double x = cubic_bezier(
                0.0, first_x, second_x, span, middle);
            if (x < offset) {
                lower = middle;
            } else {
                upper = middle;
            }
        }
        t = (lower + upper) * 0.5;
    }

    const double first_y = lo.value
        + tangent_slope(channel, lo_index, false) * out_weight;
    const double second_y = hi.value
        - tangent_slope(channel, hi_index, true) * in_weight;
    double result = cubic_bezier(
        lo.value, first_y, second_y, hi.value, t);
    const bool left_auto = lo.tangent_mode == KeyTangentMode::Auto;
    const bool right_auto = hi.interp != ChannelInterp::Cubic
        || hi.tangent_mode == KeyTangentMode::Auto;
    if (left_auto && right_auto) {
        result = std::clamp(
            result, std::min(lo.value, hi.value),
            std::max(lo.value, hi.value));
    }
    return result;
}

ChannelKeyframes SceneModel::keyframes_of(const ChannelEntry& channel) {
    ChannelKeyframes curve{};
    curve.target = channel.target;
    curve.component = channel.component;
    curve.parameter = channel.parameter;
    for (std::size_t index = 0U; index < channel.keys.size(); ++index) {
        const ChannelKey& key = channel.keys[index];
        // Keys are frame-sorted; a repeated frame keeps its first key, as the
        // editor projection does.
        if (!curve.frames.empty() && curve.frames.back() == key.frame) {
            continue;
        }
        KeyInterpolation interpolation = KeyInterpolation::Linear;
        std::optional<KeyTangent> tangent;
        switch (key.interp) {
        case ChannelInterp::Hold: interpolation = KeyInterpolation::Hold; break;
        case ChannelInterp::Linear: interpolation = KeyInterpolation::Linear; break;
        case ChannelInterp::Cubic:
            interpolation = KeyInterpolation::Cubic;
            tangent = KeyTangent{key.tangent_mode, tangent_slope(channel.keys, index, true),
                                 tangent_slope(channel.keys, index, false), key.in_weight, key.out_weight};
            break;
        }
        curve.frames.push_back(key.frame);
        curve.values.push_back(key.value);
        curve.interpolations.push_back(interpolation);
        curve.tangents.push_back(tangent);
    }
    return curve;
}

std::size_t SceneModel::load_keyframes(
    const ChannelKeyframes& channel,
    std::array<ChannelKey, max_channel_keys>& storage) noexcept {
    const std::size_t count = channel.frames.size();
    if (count == 0U || count > max_channel_keys || channel.values.size() != count)
        return 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        auto& key = storage[index];
        key.frame = channel.frames[index];
        key.value = channel.values[index];
        constexpr double maximum_value =
            static_cast<double>(std::numeric_limits<float>::max());
        if (!std::isfinite(key.value) || key.value < -maximum_value ||
            key.value > maximum_value) return 0U;
        const auto interpolation = index < channel.interpolations.size()
            ? channel.interpolations[index] : KeyInterpolation::Linear;
        switch (interpolation) {
        case KeyInterpolation::Hold: key.interp = ChannelInterp::Hold; break;
        case KeyInterpolation::Linear: key.interp = ChannelInterp::Linear; break;
        case KeyInterpolation::Cubic: key.interp = ChannelInterp::Cubic; break;
        default: return 0U;
        }
        if (interpolation == KeyInterpolation::Cubic &&
            index < channel.tangents.size() && channel.tangents[index]) {
            const KeyTangent& tangent = *channel.tangents[index];
            key.tangent_mode = tangent.mode;
            key.in_slope = tangent.in_slope;
            key.out_slope = tangent.out_slope;
            key.in_weight = tangent.in_weight;
            key.out_weight = tangent.out_weight;
            if (tangent.mode == KeyTangentMode::Aligned) {
                key.in_slope = std::midpoint(key.in_slope, key.out_slope);
                key.out_slope = key.in_slope;
            }
            if (tangent.mode > KeyTangentMode::Flat) return 0U;
        }
    }
    return valid_channel({storage.data(), count}) ? count : 0U;
}

std::optional<KeySlopes> evaluated_key_slopes(
    const ChannelKeyframes& channel, std::size_t index) noexcept {
    std::array<SceneModel::ChannelKey, max_channel_keys> storage{};
    const std::size_t count = SceneModel::load_keyframes(channel, storage);
    if (index >= count) return std::nullopt;
    const std::span<const SceneModel::ChannelKey> keys{storage.data(), count};
    return KeySlopes{SceneModel::tangent_slope(keys, index, true),
                     SceneModel::tangent_slope(keys, index, false)};
}

std::optional<double> sample_keyframes(
    const ChannelKeyframes& channel, core::i64 frame) noexcept {
    return sample_keyframes(channel, frame, 0.0);
}

std::optional<double> sample_keyframes(
    const ChannelKeyframes& channel, core::i64 frame, double fraction) noexcept {
    std::array<SceneModel::ChannelKey, max_channel_keys> storage{};
    const std::size_t count = SceneModel::load_keyframes(channel, storage);
    if (count == 0U || !(fraction >= 0.0 && fraction < 1.0)) return std::nullopt;
    return SceneModel::sample_channel({storage.data(), count}, frame, fraction);
}

std::vector<ChannelKeyframes> SceneModel::channel_keyframes() const {
    // Keyed by (target, component) so the timeline draws diamonds on the right
    // per-component row and the exact authored curve between them. Ordered by
    // the map so output is deterministic.
    struct VisibleKey final {
        core::i64 frame{};
        double value{};
        KeyInterpolation interpolation{KeyInterpolation::Linear};
        std::optional<KeyTangent> tangent;
    };
    std::map<std::tuple<doc::NodeId, PropertyChannel, std::string>, std::vector<VisibleKey>>
        by_channel;
    for (const auto& [id, entry] : entries_) {
        const auto* const channel = std::get_if<ChannelEntry>(&entry);
        if (channel == nullptr) {
            continue;
        }
        // `entries_` is node-id ordered, and evaluation lets the first channel
        // win an entire (target, property) slot. The editor query must expose
        // that same curve rather than merge otherwise-inert keys from a
        // duplicate channel into it.
        const auto [position, inserted] = by_channel.try_emplace(
            std::tuple{channel->target, channel->component, channel->parameter});
        if (!inserted) {
            continue;
        }
        auto& keys = position->second;
        for (std::size_t key_index = 0U;
             key_index < channel->keys.size(); ++key_index) {
            const auto& key = channel->keys[key_index];
            KeyInterpolation interpolation = KeyInterpolation::Linear;
            switch (key.interp) {
            case ChannelInterp::Hold:
                interpolation = KeyInterpolation::Hold;
                break;
            case ChannelInterp::Linear:
                interpolation = KeyInterpolation::Linear;
                break;
            case ChannelInterp::Cubic:
                interpolation = KeyInterpolation::Cubic;
                break;
            }
            std::optional<KeyTangent> tangent;
            if (key.interp == ChannelInterp::Cubic) {
                tangent = KeyTangent{
                    key.tangent_mode,
                    tangent_slope(channel->keys, key_index, true),
                    tangent_slope(channel->keys, key_index, false),
                    key.in_weight,
                    key.out_weight,
                };
            }
            keys.push_back(VisibleKey{
                key.frame, key.value, interpolation, tangent});
        }
    }
    std::vector<ChannelKeyframes> result;
    result.reserve(by_channel.size());
    for (auto& [identity, keys] : by_channel) {
        std::stable_sort(
            keys.begin(), keys.end(),
            [](const VisibleKey& left, const VisibleKey& right) noexcept {
                return left.frame < right.frame;
            });
        keys.erase(
            std::unique(
                keys.begin(), keys.end(),
                [](const VisibleKey& left,
                   const VisibleKey& right) noexcept {
                    return left.frame == right.frame;
                }),
            keys.end());
        ChannelKeyframes visible{};
        visible.target = std::get<0>(identity);
        visible.component = std::get<1>(identity);
        visible.parameter = std::get<2>(identity);
        visible.frames.reserve(keys.size());
        visible.values.reserve(keys.size());
        visible.interpolations.reserve(keys.size());
        visible.tangents.reserve(keys.size());
        for (const VisibleKey& key : keys) {
            visible.frames.push_back(key.frame);
            visible.values.push_back(key.value);
            visible.interpolations.push_back(key.interpolation);
            visible.tangents.push_back(key.tangent);
        }
        result.push_back(std::move(visible));
    }
    return result;
}

doc::NodeId SceneModel::effect_settings_node() const noexcept {
    for (const auto& [id, entry] : entries_) {
        if (std::holds_alternative<SettingsEntry>(entry)) {
            return id;
        }
    }
    return doc::NodeId{};
}

EffectSettings SceneModel::effect_settings() const {
    // Lowest node id wins, deterministically, exactly as the sync policy
    // resolves a document that somehow carries two.
    for (const auto& [id, entry] : entries_) {
        if (const auto* const settings = std::get_if<SettingsEntry>(&entry); settings && settings->publication_scene == SceneId{}) {
            return settings->settings;
        }
    }
    return EffectSettings{};
}

Environment SceneModel::environment(SceneId scene) const {
    const auto found = entries_.find(scene);
    if (found != entries_.end()) {
        if (const auto* stage = std::get_if<StageEntry>(&found->second)) return stage->environment;
    }
    return {};
}

RendererSettings SceneModel::renderer_settings() const {
    for (const auto& [id, entry] : entries_) {
        static_cast<void>(id);
        if (const auto* const settings = std::get_if<SettingsEntry>(&entry); settings && settings->publication_scene == SceneId{}) {
            return settings->renderer;
        }
    }
    return RendererSettings{};
}

std::vector<CameraPlacement> SceneModel::cameras(SceneId scene_id) const {
    std::vector<CameraPlacement> placements;
    if (!valid_scene(scene_id)) {
        return placements;
    }
    for (const auto& [id, entry] : entries_) {
        const auto* const camera = std::get_if<CameraEntry>(&entry);
        if (camera == nullptr ||
            camera->scene != scene_id) {
            continue;
        }
        placements.push_back(CameraPlacement{
            id, scene_id, camera->camera, camera->position, camera->rotation,
            camera->tracking_source, camera->icvfx});
    }
    return placements;
}

std::vector<TrackingSource> SceneModel::tracking_sources() const {
    std::vector<TrackingSource> sources;
    for (const auto& [id, entry] : entries_) {
        const auto* const source = std::get_if<TrackingSourceEntry>(&entry);
        if (source == nullptr) {
            continue;
        }
        if (sources.size() >= max_tracking_sources) {
            // A runaway document is truncated deterministically rather than
            // opening an unbounded set of sockets. entries_ is id-ordered, so
            // every machine keeps the same ones.
            break;
        }
        sources.push_back(TrackingSource{id, source->name, source->settings});
    }
    return sources;
}

core::u32 clip_source_frame(const ClipPlacement& clip, SceneTime time,
                            core::u64 show_frame) noexcept {
    return clip_frame_at(time, show_frame, clip.timing, clip.frame_rate,
                         clip.sequence_start, clip.sequence_count);
}

std::vector<ClipPlacement> SceneModel::clips(SceneId scene_id) const {
    std::vector<ClipPlacement> result;
    if (!valid_scene(scene_id)) {
        return result;
    }
    for (const auto& [id, entry] : entries_) {
        const auto* const clip = std::get_if<MediaEntry>(&entry);
        if (clip == nullptr ||
            clip->scene != scene_id) {
            continue;
        }
        result.push_back(ClipPlacement{
            id, scene_id, clip->source, clip->sequence_start,
            clip->sequence_count, clip->timing,
            clip->lane.value_or(doc::NodeId{}), clip->clip_order,
            clip->locked, clip->frame_rate, clip->parent_group,
            clip->group_order});
    }
    std::map<doc::NodeId, core::i64> lane_orders;
    lane_orders.emplace(doc::NodeId{}, 0);
    for (const auto& [id, entry] : entries_) {
        const auto* const lane = std::get_if<LaneEntry>(&entry);
        if (lane != nullptr && lane->scene == scene_id) {
            lane_orders.emplace(id, lane->order);
        }
    }
    std::stable_sort(
        result.begin(), result.end(),
        [&lane_orders](const ClipPlacement& left,
                       const ClipPlacement& right) noexcept {
            const auto order_of = [&lane_orders](doc::NodeId lane) noexcept {
                const auto found = lane_orders.find(lane);
                return found == lane_orders.end()
                    ? std::numeric_limits<core::i64>::max()
                    : found->second;
            };
            const core::i64 left_layer_order = left.lane == doc::NodeId{}
                ? left.group_order
                : left.order;
            const core::i64 right_layer_order = right.lane == doc::NodeId{}
                ? right.group_order
                : right.order;
            const auto left_key = std::tuple{
                order_of(left.lane), left.lane.value(), left_layer_order,
                left.timing.start, left.node.value()};
            const auto right_key = std::tuple{
                order_of(right.lane), right.lane.value(), right_layer_order,
                right.timing.start, right.node.value()};
            return left_key < right_key;
        });
    return result;
}

std::vector<TimelineMarker> SceneModel::markers(SceneId scene_id) const {
    std::vector<TimelineMarker> result;
    if (!valid_scene(scene_id)) {
        return result;
    }
    for (const auto& [id, entry] : entries_) {
        const auto* const marker = std::get_if<MarkerEntry>(&entry);
        if (marker == nullptr
            || marker->scene != scene_id) {
            continue;
        }
        result.push_back(TimelineMarker{id, scene_id, marker->frame,
                                        marker->name, marker->note,
                                        marker->color, marker->cue,
                                        marker->loop_wait_ms,
                                        marker->loop_fade_ms});
    }
    // Entries iterate in id order, so a stable sort on frame leaves equal
    // frames in id order on every machine.
    std::stable_sort(result.begin(), result.end(),
                     [](const TimelineMarker& left,
                        const TimelineMarker& right) noexcept {
                         return left.frame < right.frame;
                     });
    return result;
}

std::vector<TimelineLane> SceneModel::lanes(SceneId scene_id) const {
    std::vector<TimelineLane> result;
    if (!valid_scene(scene_id)) {
        return result;
    }
    bool has_implicit{};
    for (const auto& [id, entry] : entries_) {
        if (const auto* const lane = std::get_if<LaneEntry>(&entry)) {
            if (lane->scene == scene_id) {
                TimelineLane projected{
                    id, scene_id, lane->order, lane->name, lane->muted,
                    lane->locked, lane->compositing, false};
                projected.position = lane->position;
                projected.yaw = lane->yaw;
                projected.pitch = lane->pitch;
                projected.roll = lane->roll;
                // The lock is resolved here rather than left to the caller.
                // An Inspector drawing the authored height beside a screen
                // rendering the derived one would be showing a number that is
                // not true of anything.
                projected.scale = screen_extent(
                    lane->scale, lane->raster_width, lane->raster_height,
                    lane->aspect_locked);
                projected.opacity = lane->style.opacity;
                projected.raster_width = lane->raster_width;
                projected.raster_height = lane->raster_height;
                projected.aspect_locked = lane->aspect_locked;
                projected.sync_locked = lane->sync_locked;
                projected.vehicle = lane->vehicle;
                projected.parent = lane->parent.value_or(doc::NodeId{});
                projected.slot = lane->slot;
                projected.time_offset = lane->time_offset;
                result.push_back(std::move(projected));
            }
        } else if (const auto* const clip = std::get_if<MediaEntry>(&entry)) {
            has_implicit = has_implicit
                || (clip->scene == scene_id
                    && !clip->lane.has_value());
        }
    }
    if (has_implicit) {
        // The name is never shown: the editor draws unlaned clips as ordinary
        // top-level rows rather than under a header, because a clip outside
        // every lane is already visibly outside every lane. It owns no screen
        // either - such a clip stands on its own transform, and a nominal
        // plane here would make the projection claim otherwise.
        result.push_back(TimelineLane{
            doc::NodeId{}, scene_id, 0, {}, false, false,
            BlendMode::Normal, true});
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const TimelineLane& left, const TimelineLane& right) noexcept {
            return std::pair{left.order, left.id.value()}
                < std::pair{right.order, right.id.value()};
        });
    return result;
}

std::vector<SceneGroup> SceneModel::groups(SceneId scene_id) const {
    return resolve_groups(scene_id).groups;
}

std::vector<GroupDiagnostic> SceneModel::group_diagnostics(
    SceneId scene_id) const {
    return resolve_groups(scene_id).diagnostics;
}

std::vector<EffectInstance> SceneModel::effects() const {
    std::vector<EffectInstance> result;
    result.reserve(std::min(counters_.effects, max_scene_effects));
    for (const auto& [id, entry] : entries_) {
        const auto* const effect = std::get_if<EffectEntry>(&entry);
        if (effect == nullptr || result.size() >= max_scene_effects) {
            continue;
        }
        result.push_back(EffectInstance{id, effect->target, effect->kind,
                                        effect->order, effect->enabled,
                                        effect->value, effect->levels,
                                        effect->lift_gamma_gain,
                                        effect->contrast_pivot,
                                        effect->color_matrix,
                                        effect->white_balance,
                                        effect->curves,
                                        effect->film_grain,
                                        effect->box_blur,
                                        effect->directional_blur,
                                        effect->chroma_key,
                                        effect->luma_key});
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const EffectInstance& left,
           const EffectInstance& right) noexcept {
            if (left.target != right.target) {
                return left.target < right.target;
            }
            if (left.order != right.order) {
                return left.order < right.order;
            }
            return left.source < right.source;
        });
    return result;
}

std::vector<MaskInstance> SceneModel::masks(SceneId scene_id) const {
    const GroupProjection groups = resolve_groups(scene_id);
    return resolve_masks(scene_id, groups).masks;
}

std::vector<MaskDiagnostic> SceneModel::mask_diagnostics(
    SceneId scene_id) const {
    const GroupProjection groups = resolve_groups(scene_id);
    return resolve_masks(scene_id, groups).diagnostics;
}

Scene SceneModel::evaluate(SceneTime time, ShowLoop loop) const {
    return evaluate(default_scene_id(), time, loop);
}

Scene SceneModel::evaluate(
    SceneId scene_id, SceneTime time, ShowLoop loop,
    std::span<const ClipTimingOverride> timing_overrides,
    const SoloFilter* solo) const {
    // A placement's timing as this evaluation reads it: an override's for a
    // clip one names, the authored one for every other.
    const auto timing_of = [timing_overrides](doc::NodeId node,
                                              const MediaEntry& clip) noexcept
        -> const ClipTiming& {
        const auto found = std::lower_bound(
            timing_overrides.begin(), timing_overrides.end(), node,
            [](const ClipTimingOverride& entry, doc::NodeId id) noexcept {
                return entry.clip < id;
            });
        return found != timing_overrides.end() && found->clip == node
            ? found->timing
            : clip.timing;
    };
    Scene scene{};
    scene.media_assets.emplace();
    scene.wall_colors.assign(wall_colors_.begin(), wall_colors_.end());
    scene.id = scene_id;
    for(const auto& entry : entries_)
        if(const auto* stage=std::get_if<StageEntry>(&entry.second))
            scene.asset_refresh_generation=std::max(scene.asset_refresh_generation,stage->asset_refresh_generation);
    scene.document_version = document_version_;
    scene.temporal_document_digest = temporal_document_digest_;
    scene.show_frame = time.frame;
    scene.show_rate = {time.rate_numerator, time.rate_denominator};
    const auto media_prewarm_lead = static_cast<core::i64>(
        time.rate_denominator == 0U ? 0U :
        (static_cast<core::u64>(media_prewarm_seconds) * time.rate_numerator
         + time.rate_denominator - 1U) / time.rate_denominator);
    if (!valid_scene(scene_id)) {
        return scene;
    }
    const auto seconds = time.seconds();

    const auto stage_entry = entries_.find(scene_id);
    if (stage_entry != entries_.end()) {
        if (const auto* const stage =
                std::get_if<StageEntry>(&stage_entry->second)) {
            scene.canvas = stage->canvas;
            scene.environment = stage->environment;
            scene.asset_commit = stage->asset_commit;
            scene.publication_revision = stage->publication_revision;
        }
    }

    // The scene's camera, placed below once its channels are known.
    const CameraEntry* scene_camera{};
    for (const auto& held : entries_) {
        const auto* const camera = std::get_if<CameraEntry>(&held.second);
        if (camera == nullptr
            || scope_of(held.first) != scene_id) {
            continue;
        }
        scene.camera = camera->camera;
        scene.camera_source = held.first;
        scene_camera = camera;
        break;
    }

    // Every tracked camera in this scene: one that names a feed which actually
    // exists. Naming a feed *is* the act of making a camera a tracked camera,
    // so there is no separate "which camera is live" state to get out of step
    // with it, and a volume with three cameras on it has three of these.
    //
    // Sorted into the order their plates are composited - ascending render
    // order, then ascending node id - so where two overlap, every node in the
    // cohort puts the same one on top. The tie-break on node id matters as
    // much as the order itself: without it two cameras sharing a render order
    // would compose in whatever order the document was walked in.
    for (const auto& held : entries_) {
        if (scene.tracked_cameras.size() >= max_tracked_cameras) {
            break;
        }
        const auto* const camera = std::get_if<CameraEntry>(&held.second);
        if (camera == nullptr || camera->tracking_source.value() == 0U
            || scope_of(held.first) != scene_id) {
            continue;
        }
        const auto source = entries_.find(camera->tracking_source);
        if (source == entries_.end()
            || !std::holds_alternative<TrackingSourceEntry>(source->second)) {
            // A dangling assignment is left visible in the document for the
            // editor to diagnose, but it binds nothing: a camera pointing at a
            // deleted feed must not silently inherit another one.
            continue;
        }
        TrackedCameraView view{};
        view.camera = held.first;
        view.source = camera->tracking_source;
        view.authored = camera->camera;
        view.icvfx = camera->icvfx;
        view.calibration = clamped(
            std::get<TrackingSourceEntry>(source->second).settings)
                               .calibration;
        scene.tracked_cameras.push_back(view);
    }
    std::stable_sort(scene.tracked_cameras.begin(), scene.tracked_cameras.end(),
                     [](const TrackedCameraView& left,
                        const TrackedCameraView& right) noexcept {
                         if (left.icvfx.render_order
                             != right.icvfx.render_order) {
                             return left.icvfx.render_order
                                 < right.icvfx.render_order;
                         }
                         return left.camera.value() < right.camera.value();
                     });

    // Keyframe channels override transform components at this frame. Built once,
    // before the drawables, so each object or clip picks up its overrides as it
    // is built. Lowest-id channel wins per (target, component): entries_ is id-
    // ordered, so the first writer of a slot keeps it, matching how a duplicate
    // stage or camera resolves. Past the channel cap the rest are ignored, in id
    // order, so every node truncates identically.
    using Override =
        std::array<std::optional<float>, static_cast<std::size_t>(PropertyChannel::Count)>;
    std::map<doc::NodeId, Override> overrides;
    // Keyed particle parameters by effect and key; lowest id wins as above.
    std::map<doc::NodeId, std::map<std::string, const ChannelEntry*>> parameter_channels;
    core::u32 channels_seen = 0U;
    for (const auto& [id, entry] : entries_) {
        const auto* const channel = std::get_if<ChannelEntry>(&entry);
        if (channel == nullptr || scope_of(id) != scene_id) {
            continue;
        }
        if (channels_seen >= max_scene_channels) {
            break;
        }
        ++channels_seen;
        if (channel->component == PropertyChannel::ParticleParameter) {
            parameter_channels[channel->target].try_emplace(channel->parameter, channel);
            continue;
        }
        auto& slot =
            overrides[channel->target][static_cast<std::size_t>(channel->component)];
        if (!slot.has_value()) {
            slot = static_cast<float>(
                sample_channel(channel->keys, static_cast<core::i64>(time.frame)));
        }
    }
    const auto apply_camera_lens = [&overrides](doc::NodeId id, Camera& camera) {
        const auto found = overrides.find(id);
        if (found == overrides.end()) return;
        const auto apply = [&](PropertyChannel channel, float& field,
                               float minimum, float maximum) {
            if (const auto value = found->second[static_cast<std::size_t>(channel)];
                value.has_value() && std::isfinite(*value)) {
                field = std::clamp(*value, minimum, maximum);
            }
        };
        apply(PropertyChannel::CameraFieldOfView, camera.field_of_view, 0.001F, 0.499F);
        apply(PropertyChannel::CameraShutter, camera.shutter_seconds, 0.00001F, 1.0F);
        apply(PropertyChannel::CameraFocusDistance, camera.focus_distance_m, 0.0F, 100000.0F);
        apply(PropertyChannel::CameraAperture, camera.aperture_f_number, 0.1F, 128.0F);
    };
    apply_camera_lens(scene.camera_source, scene.camera);
    for (TrackedCameraView& view : scene.tracked_cameras) {
        apply_camera_lens(view.camera, view.authored);
    }
    // A camera follows its position and rotation channels like any element,
    // then an orbit carries it round the stage's vertical axis, turning it
    // with the sweep. Every tracked camera is placed the same way: its
    // authored pose is what it shows whenever its tracker is not live, so a
    // keyed camera must not fall back to an unkeyed one.
    const auto place_keyed = [&](const CameraEntry& entry, doc::NodeId id,
                                 Camera& camera) {
        Vec3 position = entry.position;
        Vec3 turns = entry.rotation;
        if (const auto found = overrides.find(id);
            found != overrides.end()) {
            const auto& ov = found->second;
            const auto at = [&ov](PropertyChannel channel) {
                return ov[static_cast<std::size_t>(channel)];
            };
            if (const auto v = at(PropertyChannel::PositionX)) position.x = *v;
            if (const auto v = at(PropertyChannel::PositionY)) position.y = *v;
            if (const auto v = at(PropertyChannel::PositionZ)) position.z = *v;
            if (const auto v = at(PropertyChannel::RotationYaw)) turns.x = *v;
            if (const auto v = at(PropertyChannel::RotationPitch)) turns.y = *v;
            if (const auto v = at(PropertyChannel::RotationRoll)) turns.z = *v;
        }
        if (entry.orbit_turns_per_second != 0.0F) {
            const Turns orbit = wrap_turns(
                static_cast<double>(entry.orbit_turns_per_second)
                * seconds);
            position = rotate_around(position, Vec3{0.0F, 1.0F, 0.0F}, orbit);
            turns.x += orbit;
        }
        place_camera(camera, position, turns.x, turns.y, turns.z);
    };
    if (scene_camera != nullptr) {
        place_keyed(*scene_camera, scene.camera_source, scene.camera);
    }
    for (TrackedCameraView& view : scene.tracked_cameras) {
        const auto held = entries_.find(view.camera);
        if (held == entries_.end()) {
            continue;
        }
        if (const auto* const camera = std::get_if<CameraEntry>(&held->second)) {
            place_keyed(*camera, view.camera, view.authored);
        }
    }
    for (const auto& held : entries_) {
        const auto* const camera = std::get_if<CameraEntry>(&held.second);
        if (camera == nullptr
            || scope_of(held.first) != scene_id) {
            continue;
        }
        SceneCamera placed{held.first, camera->camera};
        apply_camera_lens(placed.node, placed.camera);
        place_keyed(*camera, placed.node, placed.camera);
        scene.cameras.push_back(std::move(placed));
    }
    if (const auto found = overrides.find(scene_id); found != overrides.end()) {
        for (auto index = static_cast<std::size_t>(PropertyChannel::SkyTimeOfDay);
             index <= static_cast<std::size_t>(PropertyChannel::SkyIlluminance); ++index) {
            if (const auto value = found->second[index])
                set_environment_channel(scene.environment, static_cast<PropertyChannel>(index), *value);
        }
    }
    // Reserve the first directional slot for the physical sun so the existing
    // cascade path, fog and baked transport all follow the same solar time.
    if (scene.environment.physical_sky.enabled)
        scene.directional_lights.push_back(physical_sun(scene.environment.physical_sky, scene_id));
    const auto apply_transform_override = [&overrides](
                                              doc::NodeId id,
                                              Vec3& position,
                                              Turns& yaw,
                                              Turns& pitch,
                                              Turns& roll,
                                              Vec3& scale) {
        const auto found = overrides.find(id);
        if (found == overrides.end()) {
            return;
        }
        const auto& ov = found->second;
        const auto at = [&ov](PropertyChannel c) {
            return ov[static_cast<std::size_t>(c)];
        };
        if (const auto v = at(TransformChannel::PositionX)) {
            position.x = *v;
        }
        if (const auto v = at(TransformChannel::PositionY)) {
            position.y = *v;
        }
        if (const auto v = at(TransformChannel::PositionZ)) {
            position.z = *v;
        }
        if (const auto v = at(TransformChannel::RotationYaw)) {
            yaw = *v;
        }
        if (const auto v = at(TransformChannel::RotationPitch)) {
            pitch = *v;
        }
        if (const auto v = at(TransformChannel::RotationRoll)) {
            roll = *v;
        }
        if (const auto v = at(TransformChannel::ScaleX)) {
            scale.x = *v;
        }
        if (const auto v = at(TransformChannel::ScaleY)) {
            scale.y = *v;
        }
        if (const auto v = at(TransformChannel::ScaleZ)) {
            scale.z = *v;
        }
    };
    const auto apply_override = [&overrides, &apply_transform_override](
                                   doc::NodeId id,
                                   Vec3& position,
                                   Turns& yaw,
                                   Turns& pitch,
                                   Turns& roll,
                                   Vec3& scale,
                                   LayerStyle& style) {
        apply_transform_override(id, position, yaw, pitch, roll, scale);
        const auto found = overrides.find(id);
        if (found == overrides.end()) {
            return;
        }
        const auto& ov = found->second;
        const auto at = [&ov](PropertyChannel c) {
            return ov[static_cast<std::size_t>(c)];
        };
        if (const auto v = at(PropertyChannel::Opacity)) {
            style.opacity = std::clamp(*v, 0.0F, 1.0F);
        }
        if (const auto v = at(PropertyChannel::Brightness)) {
            style.effects.brightness = std::clamp(*v, 0.0F, 16.0F);
            style.effects.brightness_enabled = true;
        }
        if (const auto v = at(PropertyChannel::Saturation)) {
            style.effects.saturation = normalized_saturation(*v);
            style.effects.saturation_enabled = true;
        }
        if (const auto v = at(PropertyChannel::BlurRadius)) {
            style.effects.blur_radius = std::clamp(*v, 0.0F, 64.0F);
            style.effects.blur_enabled = true;
        }
    };

    std::map<doc::NodeId, GroupLocal> group_locals;
    for (const auto& [id, entry] : entries_) {
        const auto* const group = std::get_if<GroupEntry>(&entry);
        if (group == nullptr
            || group->scene != scene_id) {
            continue;
        }
        GroupLocal local{group->position, group->yaw, group->pitch,
                         group->roll, group->scale};
        apply_transform_override(id, local.position, local.yaw, local.pitch,
                                 local.roll, local.scale);
        group_locals.emplace(id, local);
    }
    const GroupProjection group_projection =
        resolve_groups(scene_id, &group_locals);
    scene.groups = group_projection.groups;
    scene.group_diagnostics = group_projection.diagnostics;
    MaskProjection mask_projection =
        resolve_masks(scene_id, group_projection);
    scene.masks = std::move(mask_projection.masks);
    scene.mask_programs = std::move(mask_projection.programs);
    scene.mask_diagnostics = std::move(mask_projection.diagnostics);

    // Evaluate first-class effects before drawables. Channels target the effect
    // node itself, so two effects of one kind remain independently animatable.
    scene.effects.reserve(std::min(counters_.effects, max_scene_effects));
    for (const auto& [id, entry] : entries_) {
        const auto* const effect = std::get_if<EffectEntry>(&entry);
        if (effect == nullptr || scope_of(id) != scene_id
            || scene.effects.size() >= max_scene_effects) {
            continue;
        }
        float value = effect->value;
        FilmGrainParameters film_grain = effect->film_grain;
        BoxBlurParameters box_blur = effect->box_blur;
        DirectionalBlurParameters directional_blur =
            effect->directional_blur;
        ChromaKeyParameters chroma_key = effect->chroma_key;
        LumaKeyParameters luma_key = effect->luma_key;
        const auto found = overrides.find(id);
        if (found != overrides.end()) {
            std::optional<PropertyChannel> property;
            switch (effect->kind) {
            case EffectKind::Brightness:
                property = PropertyChannel::Brightness;
                break;
            case EffectKind::Saturation:
                property = PropertyChannel::Saturation;
                break;
            case EffectKind::Blur:
                property = PropertyChannel::BlurRadius;
                break;
            case EffectKind::BoxBlur: {
                const auto& animated = found->second[static_cast<std::size_t>(
                    PropertyChannel::BoxBlurRadius)];
                if (animated.has_value()) {
                    // Box radius is authored as an integer. Animation samples
                    // convert to the nearest integer with half-way values away
                    // from zero, then clamp to the typed effect contract.
                    const double rounded = std::round(
                        static_cast<double>(*animated));
                    box_blur.radius_px = static_cast<core::u32>(std::clamp(
                        rounded, 0.0,
                        static_cast<double>(max_box_blur_radius_px)));
                }
                break;
            }
            case EffectKind::DirectionalBlur: {
                const auto& animated = found->second[static_cast<std::size_t>(
                    PropertyChannel::DirectionalBlurLength)];
                if (animated.has_value()) {
                    directional_blur.length_px = std::clamp(
                        *animated, 0.0F, max_directional_blur_length_px);
                }
                break;
            }
            case EffectKind::ChromaKey: {
                const auto& animated = found->second[static_cast<std::size_t>(
                    PropertyChannel::ChromaKeyMix)];
                if (animated.has_value()) {
                    chroma_key.mix = std::clamp(*animated, 0.0F, 1.0F);
                }
                break;
            }
            case EffectKind::LumaKey: {
                const auto& animated = found->second[static_cast<std::size_t>(
                    PropertyChannel::LumaKeyMix)];
                if (animated.has_value()) {
                    luma_key.mix = std::clamp(*animated, 0.0F, 1.0F);
                }
                break;
            }
            case EffectKind::Levels:
            case EffectKind::LiftGammaGain:
            case EffectKind::ContrastPivot:
            case EffectKind::ColorMatrix:
            case EffectKind::WhiteBalance:
            case EffectKind::Curves:
                break;
            case EffectKind::FilmGrain:
                property = PropertyChannel::FilmGrainAmount;
                break;
            case EffectKind::Count:
                continue;
            }
            if (property.has_value()) {
                const auto& animated = found->second[
                    static_cast<std::size_t>(*property)];
                if (animated.has_value()) {
                    if (effect->kind == EffectKind::FilmGrain) {
                        film_grain.amount = *animated;
                    } else {
                        value = *animated;
                    }
                }
            }
        }
        switch (effect->kind) {
        case EffectKind::Brightness:
            value = std::clamp(value, 0.0F, 16.0F);
            break;
        case EffectKind::Saturation:
            value = normalized_saturation(value);
            break;
        case EffectKind::Blur:
            value = std::clamp(value, 0.0F, 64.0F);
            break;
        case EffectKind::BoxBlur: {
            const auto normalized = normalized_box_blur(box_blur);
            if (!normalized.has_value()) {
                continue;
            }
            box_blur = *normalized;
            value = static_cast<float>(box_blur.radius_px);
            break;
        }
        case EffectKind::DirectionalBlur: {
            const auto normalized =
                normalized_directional_blur(directional_blur);
            if (!normalized.has_value()) {
                continue;
            }
            directional_blur = *normalized;
            value = directional_blur.length_px;
            break;
        }
        case EffectKind::ChromaKey: {
            const auto normalized = normalized_chroma_key(chroma_key);
            if (!normalized.has_value()) {
                continue;
            }
            chroma_key = *normalized;
            value = chroma_key.mix;
            break;
        }
        case EffectKind::LumaKey: {
            const auto normalized = normalized_luma_key(luma_key);
            if (!normalized.has_value()) {
                continue;
            }
            luma_key = *normalized;
            value = luma_key.mix;
            break;
        }
        case EffectKind::Levels:
        case EffectKind::LiftGammaGain:
        case EffectKind::ContrastPivot:
        case EffectKind::ColorMatrix:
        case EffectKind::WhiteBalance:
        case EffectKind::Curves:
            break;
        case EffectKind::FilmGrain: {
            const auto normalized = normalized_film_grain(film_grain);
            if (!normalized.has_value()) {
                continue;
            }
            film_grain = *normalized;
            // `value` is the scalar editor/timeline projection shared by the
            // simple amount effects. Film Grain keeps its complete typed tuple
            // alongside it, but Amount still needs the same truthful fallback
            // when no animation channel is present.
            value = film_grain.amount;
            break;
        }
        case EffectKind::Count:
            continue;
        }
        scene.effects.push_back(EffectInstance{
            id, effect->target, effect->kind, effect->order, effect->enabled,
            value, effect->levels, effect->lift_gamma_gain,
            effect->contrast_pivot, effect->color_matrix,
            effect->white_balance, effect->curves, film_grain,
            box_blur, directional_blur, chroma_key, luma_key});
    }
    std::stable_sort(
        scene.effects.begin(), scene.effects.end(),
        [](const EffectInstance& left,
           const EffectInstance& right) noexcept {
            if (left.target != right.target) {
                return left.target < right.target;
            }
            if (left.order != right.order) {
                return left.order < right.order;
            }
            return left.source < right.source;
        });
    scene.effect_settings = effect_settings();
    scene.renderer_settings = renderer_settings();
    for (const auto& [id, entry] : entries_) {
        static_cast<void>(id);
        if (const auto* settings = std::get_if<SettingsEntry>(&entry);
            settings && settings->publication_scene == scene_id) {
            scene.renderer_settings = settings->renderer;
            scene.effect_settings = settings->settings;
            break;
        }
    }
    std::map<doc::NodeId, EffectStack> folded_effects;
    std::map<doc::NodeId, double> brightness_products;
    std::map<doc::NodeId, double> saturation_products;
    for (const EffectInstance& effect : scene.effects) {
        if (!effect.enabled) {
            continue;
        }
        EffectStack& folded = folded_effects[effect.target];
        if (effect.kind == EffectKind::Brightness) {
            folded.brightness_enabled = true;
            if (effect.value != 1.0F) {
                folded.inline_operations.push_back(InlineEffectOperation{
                    EffectKind::Brightness, effect.value, {}});
            }
            auto [product, inserted] =
                brightness_products.try_emplace(effect.target, 1.0);
            static_cast<void>(inserted);
            product->second = composed_brightness_product(
                product->second, static_cast<double>(effect.value));
        } else if (effect.kind == EffectKind::Saturation) {
            folded.saturation_enabled = true;
            if (effect.value != 1.0F) {
                folded.inline_operations.push_back(InlineEffectOperation{
                    EffectKind::Saturation, effect.value, {}});
            }
            auto [product, inserted] =
                saturation_products.try_emplace(effect.target, 1.0);
            static_cast<void>(inserted);
            product->second = composed_saturation_product(
                product->second, static_cast<double>(effect.value));
        } else if (effect.kind == EffectKind::Blur) {
            folded.blur_enabled = true;
            folded.blur_radius = std::max(folded.blur_radius, effect.value);
            if (effect.value > 0.0F) {
                folded.spatial_operations.push_back(
                    SpatialEffectOperation{
                        EffectKind::Blur, effect.value, {}, {}});
            }
        } else if (effect.kind == EffectKind::BoxBlur
                   && !box_blur_is_identity(effect.box_blur)) {
            folded.spatial_operations.push_back(SpatialEffectOperation{
                EffectKind::BoxBlur, 0.0F, effect.box_blur, {}});
        } else if (effect.kind == EffectKind::DirectionalBlur
                   && !directional_blur_is_identity(
                       effect.directional_blur)) {
            folded.spatial_operations.push_back(SpatialEffectOperation{
                EffectKind::DirectionalBlur, 0.0F, {},
                effect.directional_blur});
        } else if (effect.kind == EffectKind::ChromaKey
                   && !chroma_key_is_identity(effect.chroma_key)) {
            SpatialEffectOperation operation{};
            operation.kind = EffectKind::ChromaKey;
            operation.chroma_key = effect.chroma_key;
            folded.spatial_operations.push_back(std::move(operation));
        } else if (effect.kind == EffectKind::LumaKey
                   && !luma_key_is_identity(effect.luma_key)) {
            SpatialEffectOperation operation{};
            operation.kind = EffectKind::LumaKey;
            operation.luma_key = effect.luma_key;
            folded.spatial_operations.push_back(std::move(operation));
        } else if (effect.kind == EffectKind::Levels
                   && !levels_is_identity(effect.levels)) {
            folded.inline_operations.push_back(InlineEffectOperation{
                EffectKind::Levels, 1.0F, effect.levels});
        } else if (effect.kind == EffectKind::LiftGammaGain
                   && !lift_gamma_gain_is_identity(
                       effect.lift_gamma_gain)) {
            folded.inline_operations.push_back(InlineEffectOperation{
                EffectKind::LiftGammaGain, 1.0F, {},
                effect.lift_gamma_gain});
        } else if (effect.kind == EffectKind::ContrastPivot
                   && !contrast_pivot_is_identity(
                       effect.contrast_pivot)) {
            folded.inline_operations.push_back(InlineEffectOperation{
                EffectKind::ContrastPivot, 1.0F, {}, {},
                effect.contrast_pivot});
        } else if (effect.kind == EffectKind::ColorMatrix
                   && !color_matrix_is_identity(effect.color_matrix)) {
            folded.inline_operations.push_back(InlineEffectOperation{
                EffectKind::ColorMatrix, 1.0F, {}, {}, {},
                effect.color_matrix});
        } else if (effect.kind == EffectKind::WhiteBalance
                   && !white_balance_is_identity(effect.white_balance)) {
            folded.inline_operations.push_back(InlineEffectOperation{
                EffectKind::WhiteBalance, 1.0F, {}, {}, {}, {},
                effect.white_balance});
        } else if (effect.kind == EffectKind::Curves
                   && !curves_is_identity(effect.curves)) {
            folded.inline_operations.push_back(InlineEffectOperation{
                EffectKind::Curves, 1.0F, {}, {}, {}, {}, {},
                effect.curves});
        } else if (effect.kind == EffectKind::FilmGrain
                   && !film_grain_is_identity(effect.film_grain)) {
            InlineEffectOperation operation{};
            operation.kind = EffectKind::FilmGrain;
            operation.film_grain = effect.film_grain;
            folded.inline_operations.push_back(std::move(operation));
        }
    }
    for (const auto& [target, product] : brightness_products) {
        EffectStack& folded = folded_effects[target];
        folded.brightness_product_valid = true;
        folded.brightness_product = product;
        folded.brightness = brightness_from_product(product, 1.0F);
    }
    for (const auto& [target, product] : saturation_products) {
        EffectStack& folded = folded_effects[target];
        folded.saturation_product_valid = true;
        folded.saturation_product = product;
        folded.saturation = saturation_from_product(product);
    }
    // The project's brightness response, applied once to the whole folded
    // multiplier rather than to each effect. Applying it per effect would make
    // two brightness nodes of 2 differ from one of 4, which is not what an
    // operator stacking them means. Evaluated here rather than in a shader so
    // it is part of the replicated Scene and its digest.
    const float brightness_gamma = scene.effect_settings.brightness_gamma;
    const auto fold_style = [&folded_effects, brightness_gamma](
                                doc::NodeId id, LayerStyle& style) {
        // Legacy per-drawable controls have no authored effect-node order and
        // therefore form the deterministic prefix of the first-class program.
        // Keep raw brightness here; the renderer folds each contiguous
        // brightness/saturation segment and applies response gamma once.
        if (style.effects.inline_operations.empty()) {
            if (style.effects.brightness_enabled) {
                const float brightness = style.effects.brightness_product_valid
                    ? brightness_from_product(
                          style.effects.brightness_product, 1.0F)
                    : style.effects.brightness;
                if (std::isfinite(brightness) && brightness != 1.0F) {
                    style.effects.inline_operations.push_back(
                        InlineEffectOperation{EffectKind::Brightness,
                                              brightness, {}});
                }
            }
            if (style.effects.saturation_enabled) {
                const float saturation = style.effects.saturation_product_valid
                    ? saturation_from_product(
                          style.effects.saturation_product)
                    : normalized_saturation(style.effects.saturation);
                if (saturation != 1.0F) {
                    style.effects.inline_operations.push_back(
                        InlineEffectOperation{EffectKind::Saturation,
                                              saturation, {}});
                }
            }
        }
        if (style.effects.spatial_operations.empty() &&
            style.effects.blur_enabled &&
            std::isfinite(style.effects.blur_radius) &&
            style.effects.blur_radius > 0.0F) {
            style.effects.spatial_operations.push_back(
                SpatialEffectOperation{EffectKind::Blur,
                                       style.effects.blur_radius, {}, {}});
        }
        if (style.effects.brightness_enabled) {
            const double product = style.effects.brightness_product_valid
                ? composed_brightness_product(
                      style.effects.brightness_product, 1.0)
                : (std::isfinite(style.effects.brightness)
                           ? static_cast<double>(std::clamp(
                                 style.effects.brightness, 0.0F, 16.0F))
                           : 1.0);
            style.effects.brightness_product_valid = true;
            style.effects.brightness_product = product;
        } else {
            style.effects.brightness_product_valid = false;
            style.effects.brightness_product = 1.0;
        }
        if (style.effects.saturation_enabled) {
            const double product = style.effects.saturation_product_valid
                ? composed_saturation_product(
                      style.effects.saturation_product, 1.0)
                : static_cast<double>(
                      normalized_saturation(style.effects.saturation));
            style.effects.saturation_product_valid = true;
            style.effects.saturation_product = product;
            style.effects.saturation = saturation_from_product(product);
        } else {
            style.effects.saturation_product_valid = false;
            style.effects.saturation_product = 1.0;
        }
        const auto found = folded_effects.find(id);
        if (found == folded_effects.end()) {
            if (style.effects.brightness_enabled) {
                style.effects.brightness = brightness_from_product(
                    style.effects.brightness_product, brightness_gamma);
            }
            return;
        }
        // Legacy per-layer values compose before first-class effects.
        const EffectStack& effects = found->second;
        style.effects.inline_operations.insert(
            style.effects.inline_operations.end(),
            effects.inline_operations.begin(), effects.inline_operations.end());
        style.effects.spatial_operations.insert(
            style.effects.spatial_operations.end(),
            effects.spatial_operations.begin(),
            effects.spatial_operations.end());
        if (effects.brightness_enabled) {
            const double base = style.effects.brightness_enabled
                ? style.effects.brightness_product
                : 1.0F;
            const double factor = effects.brightness_product_valid
                ? effects.brightness_product
                : static_cast<double>(effects.brightness);
            const double product =
                composed_brightness_product(base, factor);
            style.effects.brightness_enabled = true;
            style.effects.brightness_product_valid = true;
            style.effects.brightness_product = product;
        }
        if (style.effects.brightness_enabled) {
            style.effects.brightness = brightness_from_product(
                style.effects.brightness_product, brightness_gamma);
        }
        if (effects.saturation_enabled) {
            const double base = style.effects.saturation_enabled
                ? style.effects.saturation_product
                : 1.0;
            const double factor = effects.saturation_product_valid
                ? effects.saturation_product
                : static_cast<double>(effects.saturation);
            const double product =
                composed_saturation_product(base, factor);
            style.effects.saturation_enabled = true;
            style.effects.saturation_product_valid = true;
            style.effects.saturation_product = product;
            style.effects.saturation = saturation_from_product(product);
        }
        if (effects.blur_enabled) {
            const float base = style.effects.blur_enabled
                ? style.effects.blur_radius
                : 0.0F;
            style.effects.blur_enabled = true;
            style.effects.blur_radius =
                std::max(base, effects.blur_radius);
        }
    };

    scene.instances.reserve(
        std::min(counters_.objects, max_scene_instances));
    scene.media.reserve(std::min(counters_.media, max_scene_media));
    std::size_t model_capacity=counters_.models;
    for (const auto& [entry_id,entry] : entries_) {
        static_cast<void>(entry_id);
        if (const auto* model=std::get_if<ModelEntry>(&entry); model && !model->batch.empty()) model_capacity+=model->batch.size()-1;
    }
    scene.models.reserve(std::min<std::size_t>(model_capacity,max_scene_models));
    scene.particle_effects.reserve(
        std::min<std::size_t>(counters_.particle_effects, max_scene_particle_effects));
    scene.slices.reserve(std::min(counters_.slices, max_scene_slices));

    // Resolve the editorial media order before evaluating drawables. A lane
    // reference is valid only when it names a lane in this scene. Missing
    // references fail visibly by omitting the clip; an absent reference is
    // the explicit implicit-lane compatibility form.
    struct MediaProjection final {
        core::u64 order{};
        bool lane_locked{};
        BlendMode lane_compositing{BlendMode::Normal};
        // Zero for a clip standing on its own transform. Any other value is
        // the screen the clip is drawn on, and the clip's own transform is
        // not consulted at all.
        doc::NodeId lane{};
    };
    struct MediaCandidate final {
        doc::NodeId node{};
        core::i64 lane_order{};
        // The track the stack orders this under: a plate's is its take's
        // vehicle track, so a car's screens stay together in the stack.
        doc::NodeId group{};
        // Which plate of a take this is; `no_vehicle_slot` for a clip that
        // is drawn once.
        core::u8 plate{no_vehicle_slot};
        doc::NodeId lane{};
        core::i64 start{};
        core::i64 clip_order{};
        bool lane_locked{};
        BlendMode lane_compositing{BlendMode::Normal};
        const MediaEntry* clip{};
    };
    std::map<doc::NodeId, const LaneEntry*> lanes_by_id;
    for (const auto& [id, entry] : entries_) {
        const auto* const lane = std::get_if<LaneEntry>(&entry);
        if (lane != nullptr
            && lane->scene == scene_id) {
            lanes_by_id.emplace(id, lane);
        }
    }
    // Each vehicle track's slot screens, by slot. A screen naming a track
    // that is not a vehicle track in this scene belongs to nothing and shows
    // nothing; two claiming one slot leave it to the lower node id, which the
    // map's order reaches first on every machine.
    using SlotScreens = std::array<doc::NodeId, vehicle_slot_count>;
    std::map<doc::NodeId, SlotScreens> vehicle_slots;
    for (const auto& [lane_id, lane] : lanes_by_id) {
        if (lane->vehicle) {
            vehicle_slots.emplace(lane_id, SlotScreens{});
        }
    }
    for (const auto& [lane_id, lane] : lanes_by_id) {
        if (!lane->parent.has_value() || lane->slot >= vehicle_slot_count) {
            continue;
        }
        const auto vehicle = vehicle_slots.find(*lane->parent);
        if (vehicle != vehicle_slots.end()
            && vehicle->second[lane->slot] == doc::NodeId{}) {
            vehicle->second[lane->slot] = lane_id;
        }
    }
    // A show never reaches 2^63 frames; the saturation exists so the signed
    // placement arithmetic below has no undefined edge at all.
    const auto show_frame = static_cast<core::i64>(std::min(
        time.frame,
        static_cast<core::u64>(std::numeric_limits<core::i64>::max())));
    std::vector<MediaCandidate> media_candidates;
    media_candidates.reserve(counters_.media);
    for (const auto& [id, entry] : entries_) {
        const auto* const clip = std::get_if<MediaEntry>(&entry);
        if (clip == nullptr
            || clip->scene != scene_id) {
            continue;
        }
        if (!clip->visible) {
            continue;
        }
        // A solo leaves a clip out of the lane stack altogether, so the
        // clips it shows composite exactly as they would among the rest.
        if (solo != nullptr && !solo->shows(id)) {
            continue;
        }
        if (clip->parent_group.has_value()) {
            const auto parent =
                group_projection.resolved.find(*clip->parent_group);
            if (parent == group_projection.resolved.end()
                || !parent->second.visible) {
                continue;
            }
        }
        MediaCandidate candidate{};
        candidate.node = id;
        candidate.clip = clip;
        candidate.start = timing_of(id, *clip).start;
        candidate.clip_order = clip->lane.has_value()
            ? clip->clip_order
            : clip->group_order;
        if (clip->lane.has_value()) {
            const auto found = lanes_by_id.find(*clip->lane);
            if (found == lanes_by_id.end() || found->second->muted) {
                continue;
            }
            // A slot screen takes no clips of its own: what plays on it is
            // its vehicle track's take, and a second picture filed straight
            // onto it would be one layer with two sources.
            if (found->second->parent.has_value()) {
                continue;
            }
            candidate.lane_order = found->second->order;
            candidate.group = *clip->lane;
            if (const auto vehicle = vehicle_slots.find(*clip->lane);
                vehicle != vehicle_slots.end()) {
                // A take: one candidate per plate it has a screen for. A clip
                // with no plates has nothing to show on a track with no plane.
                for (std::size_t slot = 0U; slot < clip->plates.size();
                     ++slot) {
                    const doc::NodeId screen = vehicle->second[slot];
                    if (clip->plates[slot].source.empty()
                        || screen == doc::NodeId{}) {
                        continue;
                    }
                    const LaneEntry& slot_lane = *lanes_by_id.at(screen);
                    if (slot_lane.muted) {
                        continue;
                    }
                    MediaCandidate plate = candidate;
                    plate.plate = static_cast<core::u8>(slot);
                    plate.lane = screen;
                    plate.lane_locked =
                        found->second->locked || slot_lane.locked;
                    plate.lane_compositing =
                        slot_lane.compositing != BlendMode::Normal
                            ? slot_lane.compositing
                            : found->second->compositing;
                    media_candidates.push_back(plate);
                }
                continue;
            }
            candidate.lane = *clip->lane;
            candidate.lane_locked = found->second->locked;
            candidate.lane_compositing = found->second->compositing;
        }
        media_candidates.push_back(candidate);
    }
    std::stable_sort(
        media_candidates.begin(), media_candidates.end(),
        [](const MediaCandidate& left,
           const MediaCandidate& right) noexcept {
            return std::tuple{
                       left.lane_order, left.group.value(), left.plate,
                       left.clip_order, left.start, left.node.value()}
                < std::tuple{
                       right.lane_order, right.group.value(), right.plate,
                       right.clip_order, right.start, right.node.value()};
        });
    // Keyed by the clip and, for a take, which of its plates.
    using MediaKey = std::pair<doc::NodeId, core::u8>;
    std::map<MediaKey, MediaProjection> media_projection;
    // A slot screen shows one plate at a time. It is one layer - the screen
    // is what a grade, a mask and a Compositor wire address - so where two
    // takes overlap on a vehicle track, the one in front has the screen and
    // the other is not drawn there: last in the stack order above, among the
    // takes on air at this frame.
    std::map<doc::NodeId, doc::NodeId> slot_takes;
    for (std::size_t index = 0U; index < media_candidates.size(); ++index) {
        const auto& candidate = media_candidates[index];
        media_projection.emplace(
            MediaKey{candidate.node, candidate.plate},
            MediaProjection{
                static_cast<core::u64>(index), candidate.lane_locked,
                candidate.lane_compositing, candidate.lane});
        if (candidate.plate != no_vehicle_slot
            && clip_visible(timing_of(candidate.node, *candidate.clip),
                            show_frame)) {
            slot_takes.insert_or_assign(candidate.lane, candidate.node);
        }
    }

    // Each track's screen, resolved once. A track is a plane in the scene and
    // the clips filed under it are what is playing on that plane, so this is
    // the transform their pictures are drawn with - computed here, per track,
    // rather than per clip, because every clip on one screen must land on
    // exactly the same plane and a per-clip recomputation is an invitation to
    // drift.
    //
    // The screen is animatable like anything else: a channel targeting the
    // track node moves the wall, and every clip on it moves together.
    struct Screen final {
        Mat4 world{};
        LayerStyle style{};
    };
    std::map<doc::NodeId, Screen> screens;
    // A vehicle track's opacity, which every plate on its screens carries.
    std::map<doc::NodeId, float> vehicle_opacity;
    for (const auto& [lane_id, lane] : lanes_by_id) {
        auto position = lane->position;
        Turns yaw = lane->yaw;
        Turns pitch = lane->pitch;
        Turns roll = lane->roll;
        Vec3 scale = lane->scale;
        LayerStyle style = lane->style;
        apply_override(lane_id, position, yaw, pitch, roll, scale, style);
        if (lane->vehicle) {
            // No plane of its own, so no screen: what it contributes is its
            // opacity and its effects, to the plates on its slot screens.
            vehicle_opacity.emplace(lane_id, style.opacity);
            continue;
        }
        fold_style(lane_id, style);
        // A slot screen is switched off and locked with its vehicle track.
        const LaneEntry* vehicle{};
        if (lane->parent.has_value()) {
            const auto found = lanes_by_id.find(*lane->parent);
            if (found != lanes_by_id.end() && found->second->vehicle) {
                vehicle = found->second;
            }
        }
        // The lock is applied after channels, so an animated width carries its
        // height with it rather than animating the screen out of shape.
        const auto extent = screen_extent(
            scale, lane->raster_width, lane->raster_height,
            lane->aspect_locked);
        const auto world = multiply(
            translation(position),
            multiply(rotation(yaw, pitch, roll), scaling(extent)));
        screens.emplace(lane_id, Screen{world, style});
        scene.screens.push_back(SceneScreen{
            lane_id, scene_id, lane->name, lane->order, world,
            lane->raster_width, lane->raster_height, lane->aspect_locked,
            style, lane->muted || (vehicle != nullptr && vehicle->muted),
            lane->locked || (vehicle != nullptr && vehicle->locked)});
    }
    // Deterministic and matching the timeline's own stacking, so two nodes
    // digest the same list and the editor's row order is the scene's order.
    std::sort(scene.screens.begin(), scene.screens.end(),
              [](const SceneScreen& left, const SceneScreen& right) noexcept {
                  return std::pair{left.order, left.node.value()}
                      < std::pair{right.order, right.node.value()};
              });

    for (const auto& [id, entry] : entries_) {
        if (const auto* const object = std::get_if<ObjectEntry>(&entry)) {
            if (scope_of(id) != scene_id ||
                (solo != nullptr && !solo->shows(id))) {
                continue;
            }
            const GroupResolution* parent = nullptr;
            if (!object->visible) {
                continue;
            }
            if (object->parent_group.has_value()) {
                const auto found =
                    group_projection.resolved.find(*object->parent_group);
                if (found == group_projection.resolved.end()
                    || !found->second.visible) {
                    continue;
                }
                parent = &found->second;
            }
            if (scene.instances.size() >= max_scene_instances) {
                continue;
            }
            // Channels replace the base transform components; the procedural
            // spin and bob still apply on top of whatever the channel set.
            auto position = object->position;
            Turns yaw_base = object->yaw;
            Turns pitch = object->pitch;
            Turns roll = object->roll;
            Vec3 scale = object->scale;
            LayerStyle style = object->style;
            apply_override(id, position, yaw_base, pitch, roll, scale, style);
            fold_style(id, style);

            const auto spin = static_cast<double>(object->spin_turns_per_second)
                * seconds;
            const auto yaw = wrap_turns(static_cast<double>(yaw_base) + spin);

            if (object->bob_amplitude != 0.0F) {
                const auto phase = wrap_turns(
                    static_cast<double>(object->bob_turns_per_second) * seconds);
                position.y += object->bob_amplitude
                    * deterministic::sin_turns(phase);
            }

            const auto local = multiply(
                translation(position),
                multiply(rotation(yaw, pitch, roll), scaling(scale)));
            const auto world = parent == nullptr
                ? local
                : multiply(parent->world, local);
            scene.instances.push_back(MeshInstance{
                object->mesh,
                world,
                normal_basis(world),
                object->material,
                id,
                style,
                object->locked || (parent != nullptr && parent->locked),
                object->parent_group,
                object->group_order, object->bake_static});
            continue;
        }

        if (const auto* const clip = std::get_if<MediaEntry>(&entry)) {
            if (scope_of(id) != scene_id) {
                continue;
            }
            // One picture of the clip: the clip itself, or one plate of a
            // take. Everything below is the same for both but what is named
            // here - the file, where in it the clip's first frame falls, and
            // for a plate the vehicle track whose grade and opacity it also
            // carries.
            struct Picture final {
                const std::string* source{};
                doc::NodeId asset{};
                core::u32 first{};
                core::u32 count{};
                ClipTiming timing{};
                core::u8 plate{no_vehicle_slot};
                doc::NodeId vehicle{};
            };
            const auto place = [&](const MediaProjection& projection,
                                   const Picture& picture) {
                const ClipTiming& timing = picture.timing;
                const bool is_plate = picture.plate != no_vehicle_slot;
                scene.media_assets->push_back({picture.asset, *picture.source});
                const GroupResolution* parent = nullptr;
                if (!clip->visible) {
                    return;
                }
                if (clip->parent_group.has_value()) {
                    const auto found =
                        group_projection.resolved.find(*clip->parent_group);
                    if (found == group_projection.resolved.end()
                        || !found->second.visible) {
                        return;
                    }
                    parent = &found->second;
                }
                // Placement is decided before the capacity check, so a timeline
                // may hold far more clips than one frame can draw as long as few
                // of them are on air at once.
                // What this clip will want shortly, named so it can be decoded
                // before it is asked for. Reaching an arbitrary frame of a
                // long-GOP file means seeking back to a keyframe and walking
                // forward, which is several show frames of work; a clip first
                // asked on the frame it needs answers late, and the wall holds a
                // frame from somewhere else or nothing at all.
                //
                // Two moments break continuity, and both are invisible to the
                // renderer until they have already happened. The playhead reaching
                // a clip that is not on air yet is one. The show's own range
                // wrapping is the other, and it is the worse of the two: a clip
                // still on air across the wrap is not cut to, so nothing marks the
                // discontinuity, yet its source is rarely the length of the show
                // and the wrap therefore lands it somewhere unrelated in its own
                // file. Each is warmed with the frame wanted first on the far side.
                if (picture.count != 0U) {
                    const auto warm = [&](core::u64 at) {
                        const core::u64 distance = at > time.frame ? at - time.frame
                            : loop.last >= time.frame ? loop.last - time.frame + 1U : 0U;
                        MediaPrewarm request{
                            *picture.source,
                            clip_frame_at(
                                time, at, timing, clip->frame_rate,
                                picture.first, picture.count),
                            id, picture.asset, picture.first,
                            picture.count, distance, clip->resolution,
                            is_plate ? plate_stream(id, picture.plate) : 0U};
                        const auto position = std::lower_bound(
                            scene.media_prewarm.begin(), scene.media_prewarm.end(), request,
                            [](const MediaPrewarm& a, const MediaPrewarm& b) {
                                if (a.frames_until_start != b.frames_until_start)
                                    return a.frames_until_start < b.frames_until_start;
                                return a.source_node < b.source_node;
                            });
                        if (scene.media_prewarm.size() < max_media_prewarm_clips
                            || position != scene.media_prewarm.end()) {
                            scene.media_prewarm.insert(position, std::move(request));
                            if (scene.media_prewarm.size() > max_media_prewarm_clips)
                                scene.media_prewarm.pop_back();
                        }
                    };
                    if (clip_starts_within(
                            timing, show_frame, media_prewarm_lead)) {
                        warm(static_cast<core::u64>(timing.start));
                    }
                    if (show_wraps_within(
                            loop, show_frame, media_prewarm_lead)) {
                        const auto after = static_cast<core::i64>(std::min(
                            loop.first,
                            static_cast<core::u64>(
                                std::numeric_limits<core::i64>::max())));
                        if (clip_visible(timing, after)) {
                            warm(loop.first);
                        }
                    }
                }
                if (!clip_visible(timing, show_frame)) {
                    return;
                }
                const auto screen = screens.find(projection.lane);
                const bool on_screen = screen != screens.end();
                if (is_plate) {
                    // Its screen shows one take at a time: the one in front.
                    const auto showing = slot_takes.find(projection.lane);
                    if (!on_screen || showing == slot_takes.end()
                        || showing->second != id) {
                        return;
                    }
                }
                if (scene.media.size() >= max_scene_media) {
                    return;
                }
                auto position = clip->position;
                Turns yaw = clip->yaw;
                Turns pitch = clip->pitch;
                Turns roll = clip->roll;
                Vec3 scale = clip->scale;
                LayerStyle style = clip->style;
                apply_override(id, position, yaw, pitch, roll, scale, style);
                fold_style(id, style);
                if (on_screen) {
                    // The track's grade runs after the clip's, because the screen
                    // is what the clip is playing on: the operator who set a look
                    // on the wall means it to survive whichever plate is up.
                    fold_style(projection.lane, style);
                    style.opacity *= screen->second.style.opacity;
                }
                if (is_plate) {
                    // And the vehicle track's after its screen's: the take's own
                    // look, then that one wall's, then the whole car's.
                    fold_style(picture.vehicle, style);
                    const auto whole = vehicle_opacity.find(picture.vehicle);
                    if (whole != vehicle_opacity.end()) {
                        style.opacity *= whole->second;
                    }
                }
                // A non-Normal clip mode is an explicit per-placement choice. A
                // Normal clip inherits its lane's compositing mode, making the
                // lane setting useful without erasing authored clip style.
                if (style.blend == BlendMode::Normal) {
                    style.blend = projection.lane_compositing;
                }
                const auto local = multiply(
                    translation(position),
                    multiply(rotation(yaw, pitch, roll), scaling(scale)));
                // A clip on a screen is drawn on that screen. Its own transform
                // is left in the document untouched and simply not read, so a
                // clip dragged out of a track returns to where it was rather than
                // to the origin. Group membership still decides whether the clip
                // is visible and whether it is locked; it does not get to move a
                // picture that is on a wall somewhere else.
                const auto world = on_screen
                    ? screen->second.world
                    : (parent == nullptr ? local
                                         : multiply(parent->world, local));
                const auto frame_index = clip_frame_at(
                    time, time.frame, timing, clip->frame_rate,
                    picture.first, picture.count);
                // The head and tail ramps multiply whatever opacity was authored
                // or keyed, so a dissolve composes with an animated layer instead
                // of replacing its animation.
                if (timing.placed()) {
                    style.opacity *= clip_fade(timing, show_frame);
                }
                scene.media.push_back(MediaInstance{
                    clip->mesh,
                    world,
                    normal_basis(world),
                    *picture.source,
                    frame_index,
                    picture.first,
                    picture.count,
                    picture.asset,
                    // A plate is its slot screen's picture: the screen is the
                    // layer, and the take that put it there is the placement.
                    is_plate ? projection.lane : id,
                    projection.lane,
                    projection.order,
                    style,
                    clip->locked || projection.lane_locked
                        || (parent != nullptr && parent->locked),
                    clip->parent_group,
                    clip->group_order,
                    false,
                    clip->resolution,
                    is_plate ? id : doc::NodeId{},
                    is_plate ? plate_stream(id, picture.plate) : 0U});
            };

            const ClipTiming& clip_timing = timing_of(id, *clip);
            const auto take = clip->lane.has_value()
                ? vehicle_slots.find(*clip->lane) : vehicle_slots.end();
            if (take == vehicle_slots.end()) {
                const auto projection = media_projection.find(
                    MediaKey{id, no_vehicle_slot});
                if (projection != media_projection.end()) {
                    place(projection->second,
                          Picture{&clip->source, clip->asset,
                                  clip->sequence_start, clip->sequence_count,
                                  clip_timing, no_vehicle_slot, doc::NodeId{}});
                }
                continue;
            }
            // A take. Its own trim is the front plate's: every other plate
            // starts as far from that as its sync offset is from the front
            // plate's, and then by whatever its screen adds. A plate asked
            // for frames before its first holds its first.
            const core::i64 front_offset =
                clip->plates.empty() ? 0 : clip->plates.front().offset;
            for (std::size_t slot = 0U; slot < clip->plates.size(); ++slot) {
                const MediaEntry::Plate& plate = clip->plates[slot];
                const auto projection = media_projection.find(
                    MediaKey{id, static_cast<core::u8>(slot)});
                if (plate.source.empty()
                    || projection == media_projection.end()) {
                    continue;
                }
                ClipTiming plate_timing = clip_timing;
                if (plate_timing.source_in >= 0) {
                    // Both terms are bounded far inside the signed range at
                    // parse, so only the sum with the trim can leave it.
                    const core::i64 shift = plate.offset - front_offset
                        + lanes_by_id.at(projection->second.lane)->time_offset;
                    plate_timing.source_in = shift <= 0
                        ? std::max<core::i64>(
                              0, plate_timing.source_in + shift)
                        : (plate_timing.source_in
                                   > std::numeric_limits<core::i64>::max()
                                       - shift
                               ? std::numeric_limits<core::i64>::max()
                               : plate_timing.source_in + shift);
                }
                place(projection->second,
                      Picture{&plate.source, plate.asset, 0U, plate.count,
                              plate_timing, static_cast<core::u8>(slot),
                              *clip->lane});
            }
            continue;
        }

        if (const auto* const model = std::get_if<ModelEntry>(&entry)) {
            if (scope_of(id) != scene_id ||
                (solo != nullptr && !solo->shows(id))) {
                continue;
            }
            const GroupResolution* parent = nullptr;
            if (!model->visible) {
                continue;
            }
            if (model->parent_group.has_value()) {
                const auto found =
                    group_projection.resolved.find(*model->parent_group);
                if (found == group_projection.resolved.end()
                    || !found->second.visible) {
                    continue;
                }
                parent = &found->second;
            }
            if (scene.models.size() >= max_scene_models) {
                continue;
            }
            auto position = model->position;
            Turns yaw = model->yaw;
            Turns pitch = model->pitch;
            Turns roll = model->roll;
            Vec3 scale = model->scale;
            LayerStyle style = model->style;
            apply_override(id, position, yaw, pitch, roll, scale, style);
            fold_style(id, style);
            const auto local = multiply(
                translation(position),
                multiply(rotation(yaw, pitch, roll), scaling(scale)));
            const auto affine_local = multiply(local, model->affine);
            const auto world = model->linked_world.value_or(parent == nullptr
                ? affine_local
                : multiply(parent->world, affine_local));
            const auto append = [&](const Mat4& placed, doc::NodeId motion) {
              scene.models.push_back(ModelInstance{
                placed, normal_basis(placed), model->source, id,
                model->material, style,
                model->locked || (parent != nullptr && parent->locked),
                model->parent_group, model->group_order, model->bake_static, model->animation,
                model->material_override_mask, motion, motion.value() ? world : Mat4{}});
            };
            if (model->batch.empty()) append(world,{});
            else for (const auto& local_instance : model->batch) {
                if (scene.models.size() >= max_scene_models) break;
                append(multiply(world,local_instance),batch_motion_identity(id,local_instance));
            }
            continue;
        }

        if (const auto* const effect = std::get_if<ParticleEffectEntry>(&entry)) {
            if (scope_of(id) != scene_id || !effect->visible ||
                (solo != nullptr && !solo->shows(id)) ||
                scene.particle_effects.size() >= max_scene_particle_effects) {
                continue;
            }
            const GroupResolution* parent = nullptr;
            if (effect->parent_group.has_value()) {
                const auto found = group_projection.resolved.find(*effect->parent_group);
                if (found == group_projection.resolved.end() || !found->second.visible) {
                    continue;
                }
                parent = &found->second;
            }
            // Keyframed and dragged transforms apply exactly as to a model.
            auto position = effect->position;
            Turns yaw = effect->yaw;
            Turns pitch = effect->pitch;
            Turns roll = effect->roll;
            Vec3 scale = effect->scale;
            LayerStyle style{};
            apply_override(id, position, yaw, pitch, roll, scale, style);
            const auto local = multiply(translation(position), multiply(rotation(yaw, pitch, roll), scaling(scale)));
            ParticleEffect placed{
                parent == nullptr ? local : multiply(parent->world, local), effect->source, id, effect->seed,
                effect->start_frame, effect->locked || (parent != nullptr && parent->locked), effect->parent_group,
                effect->group_order, {}};
            // Every authored or keyed component, by key. The renderer samples
            // a keyed one at each simulation step, so the curve travels whole.
            const auto keyed = parameter_channels.find(id);
            std::map<std::string_view, ParticleParameter> merged;
            for (const auto& [key, base] : effect->parameters) {
                merged[key].base = base;
            }
            if (keyed != parameter_channels.end()) {
                for (const auto& [key, channel] : keyed->second) {
                    merged[key].curve = keyframes_of(*channel);
                }
            }
            for (auto& [key, parameter] : merged) {
                if (placed.parameters.size() >= max_particle_parameters) {
                    break;
                }
                parameter.key = std::string{key};
                placed.parameters.push_back(std::move(parameter));
            }
            scene.particle_effects.push_back(std::move(placed));
            continue;
        }

        if (const auto* const authored = std::get_if<LightEntry>(&entry)) {
            if (scope_of(id) != scene_id || !authored->visible ||
                (solo != nullptr && solo->lights && !solo->shows(id))) {
                continue;
            }
            // Keyframe channels animate a light's position (a local light's
            // only placement), its output and its colour, the same way they
            // animate a drawable's transform.
            const LightEntry* light = authored;
            LightEntry animated_light;
            if (const auto found = overrides.find(id); found != overrides.end()) {
                animated_light = *authored;
                const auto& ov = found->second;
                const auto at = [&ov](PropertyChannel c) {
                    return ov[static_cast<std::size_t>(c)];
                };
                if (!animated_light.directional) {
                    if (const auto v = at(PropertyChannel::PositionX)) animated_light.vector.x = *v;
                    if (const auto v = at(PropertyChannel::PositionY)) animated_light.vector.y = *v;
                    if (const auto v = at(PropertyChannel::PositionZ)) animated_light.vector.z = *v;
                }
                if (const auto v = at(PropertyChannel::LightIntensity)) {
                    animated_light.intensity = std::max(*v, 0.0F);
                }
                if (const auto v = at(PropertyChannel::LightColorR)) animated_light.color.x = std::max(*v, 0.0F);
                if (const auto v = at(PropertyChannel::LightColorG)) animated_light.color.y = std::max(*v, 0.0F);
                if (const auto v = at(PropertyChannel::LightColorB)) animated_light.color.z = std::max(*v, 0.0F);
                if (const auto v = at(PropertyChannel::LightRange)) {
                    animated_light.range = std::max(*v, 0.001F);
                }
                light = &animated_light;
            }
            if (light->directional) {
                if (scene.directional_lights.size()
                    < max_scene_directional_lights) {
                    scene.directional_lights.push_back(DirectionalLight{
                        normalize(light->vector, Vec3{0.0F, -1.0F, 0.0F}),
                        light->color,
                        light->intensity,
                        id,
                        light->locked,
                        light->unit,
                        light->angular_radius,
                        light->color_temperature_kelvin,
                        light->use_color_temperature, light->bake_indirect});
                }
            } else if (scene.point_lights.size() < max_scene_point_lights) {
                scene.point_lights.push_back(PointLight{
                    light->vector,
                    light->color,
                    light->intensity,
                    light->range,
                    id,
                    light->locked,
                    light->local_kind,
                    light->unit,
                    normalize(light->direction,
                              Vec3{0.0F, -1.0F, 0.0F}),
                    light->inner_cone,
                    light->outer_cone,
                    light->width,
                    light->height,
                    light->two_sided,
                    light->color_temperature_kelvin,
                    light->use_color_temperature,
                    light->casts_shadow,
                    light->shadow_tier, light->bake_indirect, light->ies_source, light->ies_rotation, light->radius, light->area_rotation});
            }
            continue;
        }

        if (const auto* const slice = std::get_if<SliceEntry>(&entry)) {
            if (scene.slices.size() < max_scene_slices) {
                scene.slices.push_back(slice->binding);
            }
        }
    }

    // Grouped by mesh so the renderer can draw one batch per distinct mesh,
    // and stably so that instances sharing a mesh stay in node id order. Both
    // halves are part of what two nodes have to agree on, which is why this is
    // here rather than in the renderer.
    std::stable_sort(
        scene.instances.begin(),
        scene.instances.end(),
        [](const MeshInstance& left, const MeshInstance& right) noexcept {
            return left.mesh < right.mesh;
        });
    std::stable_sort(
        scene.media.begin(), scene.media.end(),
        [](const MediaInstance& left, const MediaInstance& right) noexcept {
            return left.compositing_order < right.compositing_order;
        });

    return scene;
}

void write_renderer_settings(
    doc::Node& node, const RendererSettings& source) {
    const RendererSettings renderer = clamped(source);
    node.properties.emplace(std::string{schema::frame_budget_us}, doc::PropertyValue{static_cast<std::int64_t>(renderer.frame_budget_us)});
    node.properties.emplace(std::string{schema::photometric_scale},
                            doc::PropertyValue{static_cast<double>(renderer.photometric_scale)});
    node.properties.emplace(
        std::string{schema::renderer_backend},
        doc::PropertyValue{std::string{
            renderer.backend == RendererBackend::Advanced
                ? schema::advanced_renderer
                : schema::basic_renderer}});
    node.properties.emplace(std::string{schema::renderer_required},
                            doc::PropertyValue{renderer.required});
    const std::string_view quality =
        renderer.quality == scene::RendererQuality::Interactive
        ? schema::interactive_quality
        : renderer.quality == scene::RendererQuality::Cinematic
        ? schema::cinematic_quality
        : schema::balanced_quality;
    node.properties.emplace(
        std::string{schema::renderer_quality},
        doc::PropertyValue{std::string{quality}});
    node.properties.emplace(std::string{schema::ray_tracing_enabled},
                            doc::PropertyValue{renderer.ray_tracing});
    node.properties.emplace(std::string{schema::ddgi_enabled},
                            doc::PropertyValue{renderer.ddgi});
    node.properties.emplace(std::string{schema::restir_enabled},
                            doc::PropertyValue{renderer.restir});
    node.properties.emplace(std::string{schema::denoising_enabled},
                            doc::PropertyValue{renderer.denoising});
    node.properties.emplace(std::string{schema::taau_enabled},
                            doc::PropertyValue{renderer.taau});
    node.properties.emplace(std::string{schema::plate_depth_of_field}, doc::PropertyValue{renderer.plate_depth_of_field});
    node.properties.emplace(std::string{schema::plate_dof_radius}, doc::PropertyValue{static_cast<std::int64_t>(renderer.plate_dof_radius)});
    node.properties.emplace(std::string{schema::motion_blur_enabled},
                            doc::PropertyValue{renderer.motion_blur});
    node.properties.emplace(std::string{schema::motion_blur_radius},
                            doc::PropertyValue{static_cast<std::int64_t>(renderer.motion_blur_radius)});
    node.properties.emplace(
        std::string{schema::spatial_antialiasing_enabled},
        doc::PropertyValue{renderer.spatial_antialiasing});
    node.properties.emplace(
        std::string{schema::taa_sequence_length},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.taa_sequence_length)});
    node.properties.emplace(
        std::string{schema::ddgi_probe_count_x},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.ddgi_probe_count_x)});
    node.properties.emplace(
        std::string{schema::ddgi_probe_count_y},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.ddgi_probe_count_y)});
    node.properties.emplace(
        std::string{schema::ddgi_probe_count_z},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.ddgi_probe_count_z)});
    node.properties.emplace(
        std::string{schema::ddgi_rays_per_probe},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.ddgi_rays_per_probe)});
    node.properties.emplace(
        std::string{schema::slice_apron_limit},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.slice_apron_limit)});
    node.properties.emplace(std::string{schema::frustum_culling}, doc::PropertyValue{renderer.frustum_culling});
    node.properties.emplace(std::string{schema::geometry_instancing}, doc::PropertyValue{renderer.geometry_instancing});
    node.properties.emplace(std::string{schema::model_lod}, doc::PropertyValue{renderer.model_lod});
    node.properties.emplace(std::string{schema::texture_streaming}, doc::PropertyValue{renderer.texture_streaming});
    node.properties.emplace(std::string{schema::texture_budget_mib}, doc::PropertyValue{static_cast<std::int64_t>(renderer.texture_budget_mib)});
    node.properties.emplace(std::string{schema::texture_mip_bias}, doc::PropertyValue{static_cast<std::int64_t>(renderer.texture_mip_bias)});
    node.properties.emplace(std::string{schema::texture_anisotropy}, doc::PropertyValue{static_cast<std::int64_t>(renderer.texture_anisotropy)});
    node.properties.emplace(std::string{schema::texture_lod_bias}, doc::PropertyValue{static_cast<double>(renderer.texture_lod_bias)});
    node.properties.emplace(std::string{schema::shadow_blend_band},
        doc::PropertyValue{static_cast<double>(renderer.shadow_blend_band)});
    node.properties.emplace(std::string{schema::shadow_filter_radius},
        doc::PropertyValue{static_cast<double>(renderer.shadow_filter_radius)});
    node.properties.emplace(std::string{schema::contact_shadow_length},
        doc::PropertyValue{static_cast<double>(renderer.contact_shadow_length)});
    node.properties.emplace(std::string{schema::contact_shadow_thickness},
        doc::PropertyValue{static_cast<double>(renderer.contact_shadow_thickness)});
    node.properties.emplace(
        std::string{schema::shadow_cascades},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.shadow_cascades)});
    node.properties.emplace(
        std::string{schema::gi_strength},
        doc::PropertyValue{static_cast<double>(renderer.gi_strength)});
    node.properties.emplace(
        std::string{schema::screen_occlusion_radius},
        doc::PropertyValue{static_cast<double>(renderer.occlusion_radius)});
    node.properties.emplace(
        std::string{schema::screen_occlusion_strength},
        doc::PropertyValue{static_cast<double>(renderer.occlusion_strength)});
    node.properties.emplace(
        std::string{schema::bloom_strength},
        doc::PropertyValue{static_cast<double>(renderer.bloom_strength)});
    node.properties.emplace(
        std::string{schema::display_transform},
        doc::PropertyValue{std::string{
            renderer.color.display_transform == DisplayTransform::Srgb
                ? schema::srgb_display_transform
                : renderer.color.display_transform == DisplayTransform::Aces13
                ? schema::aces13_display_transform
                : schema::aces_display_transform}});
    node.properties.emplace(std::string{schema::show_lut},
                            doc::PropertyValue{renderer.color.show_lut_source});
    node.properties.emplace(std::string{schema::output_dither},
                            doc::PropertyValue{renderer.color.dither});
    node.properties.emplace(
        std::string{schema::inner_frustum_enabled},
        doc::PropertyValue{renderer.icvfx.inner_frustum});
    node.properties.emplace(
        std::string{schema::inner_frustum_margin_px},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.icvfx.camera_defaults.margin_px)});
    node.properties.emplace(
        std::string{schema::inner_frustum_soft_edge_px},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.icvfx.camera_defaults.soft_edge_px)});
    node.properties.emplace(
        std::string{schema::tracking_latency_frames},
        doc::PropertyValue{
            static_cast<std::int64_t>(renderer.icvfx.latency_frames)});
    node.properties.emplace(std::string{schema::tracking_pose_prediction},
                            doc::PropertyValue{renderer.icvfx.pose_prediction});
    node.properties.emplace(std::string{schema::tracking_prediction_window},
        doc::PropertyValue{static_cast<std::int64_t>(renderer.icvfx.prediction_window)});
    node.properties.emplace(std::string{schema::tracking_jitter_filter_percent},
        doc::PropertyValue{static_cast<std::int64_t>(renderer.icvfx.jitter_filter_percent)});
    node.properties.emplace(
        std::string{schema::inner_frustum_scale_numerator},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.icvfx.camera_defaults.resolution_numerator)});
    node.properties.emplace(
        std::string{schema::inner_frustum_scale_denominator},
        doc::PropertyValue{static_cast<std::int64_t>(
            renderer.icvfx.camera_defaults.resolution_denominator)});
}

} // namespace mediaserver::scene
