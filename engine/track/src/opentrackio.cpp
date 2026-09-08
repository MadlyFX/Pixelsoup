#include <mediaserver/track/opentrackio.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>

namespace mediaserver::track {
namespace {

using Result = core::Result<OpenTrackIOSample, TrackerParseError>;

// 10^0 .. 10^22 are the powers of ten that are exactly representable in
// binary64. Scaling by a table entry is therefore one correctly rounded
// multiply, which is identical on every machine that follows IEEE-754.
constexpr std::array<double, 23U> exact_powers_of_ten{{
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
}};
constexpr int max_decimal_exponent = 22;

struct Vec3d final {
    double x{};
    double y{};
    double z{};
};

struct TransformEntry final {
    Vec3d translation{};
    // Degrees, as the protocol reports them.
    double pan{};
    double tilt{};
    double roll{};
    bool has_translation{};
    bool has_rotation{};
    bool is_camera{};
};

// A strict, allocation-free JSON reader over one datagram.
//
// It is a validating reader rather than a scanner: every value is parsed to the
// end whether or not anybody wants it, so a truncated document fails at the
// point of truncation instead of being accepted because the interesting fields
// happened to arrive first.
class Reader final {
public:
    explicit Reader(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] std::optional<TrackerParseError> error() const noexcept {
        return error_;
    }

    void skip_whitespace() noexcept {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool at_end() noexcept {
        skip_whitespace();
        return position_ >= text_.size();
    }

    [[nodiscard]] char peek() noexcept {
        skip_whitespace();
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        if (peek() != expected) {
            fail(TrackerParseError::UnrecognizedMessage);
            return false;
        }
        ++position_;
        return true;
    }

    void fail(TrackerParseError reason) noexcept {
        if (!error_.has_value()) {
            error_ = reason;
        }
    }

    // Reads a JSON string. Escapes are validated but only the unescaped form is
    // returned, and a key or value containing one is reported as not matching
    // anything this parser looks for. No OpenTrackIO key or enumerated value
    // needs an escape, so that costs nothing and keeps the reader allocation
    // free.
    [[nodiscard]] std::string_view read_string() noexcept {
        if (!consume('"')) {
            return {};
        }
        const std::size_t begin = position_;
        bool escaped = false;
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == '\\') {
                escaped = true;
                if (position_ + 1U >= text_.size()) {
                    break;
                }
                const char next = text_[position_ + 1U];
                if (next == 'u') {
                    if (position_ + 5U >= text_.size()) {
                        break;
                    }
                    for (std::size_t index = 2U; index <= 5U; ++index) {
                        if (!is_hex(text_[position_ + index])) {
                            fail(TrackerParseError::UnrecognizedMessage);
                            return {};
                        }
                    }
                    position_ += 6U;
                    continue;
                }
                if (next != '"' && next != '\\' && next != '/' && next != 'b'
                    && next != 'f' && next != 'n' && next != 'r'
                    && next != 't') {
                    fail(TrackerParseError::UnrecognizedMessage);
                    return {};
                }
                position_ += 2U;
                continue;
            }
            if (c == '"') {
                const std::string_view value =
                    text_.substr(begin, position_ - begin);
                ++position_;
                return escaped ? std::string_view{} : value;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                fail(TrackerParseError::UnrecognizedMessage);
                return {};
            }
            ++position_;
        }
        fail(TrackerParseError::Truncated);
        return {};
    }

    // The JSON number grammar, read into a binary64 without consulting the C
    // library. Refuses anything outside the exactly scalable exponent range.
    [[nodiscard]] double read_number() noexcept {
        skip_whitespace();
        bool negative = false;
        if (position_ < text_.size() && text_[position_] == '-') {
            negative = true;
            ++position_;
        }
        core::u64 mantissa = 0U;
        int significant_digits = 0;
        int exponent = 0;
        bool any_digit = false;

        if (position_ < text_.size() && text_[position_] == '0') {
            ++position_;
            any_digit = true;
            // JSON forbids a leading zero followed by another digit.
            if (position_ < text_.size() && is_digit(text_[position_])) {
                fail(TrackerParseError::UnrecognizedMessage);
                return 0.0;
            }
        } else {
            while (position_ < text_.size() && is_digit(text_[position_])) {
                any_digit = true;
                accumulate(text_[position_], mantissa, significant_digits,
                           exponent, 1);
                ++position_;
            }
        }
        if (!any_digit) {
            fail(TrackerParseError::UnrecognizedMessage);
            return 0.0;
        }

        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            bool fraction_digit = false;
            while (position_ < text_.size() && is_digit(text_[position_])) {
                fraction_digit = true;
                accumulate(text_[position_], mantissa, significant_digits,
                           exponent, 0);
                ++position_;
            }
            if (!fraction_digit) {
                fail(TrackerParseError::UnrecognizedMessage);
                return 0.0;
            }
        }

        if (position_ < text_.size()
            && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            bool exponent_negative = false;
            if (position_ < text_.size()
                && (text_[position_] == '+' || text_[position_] == '-')) {
                exponent_negative = text_[position_] == '-';
                ++position_;
            }
            if (position_ >= text_.size() || !is_digit(text_[position_])) {
                fail(TrackerParseError::UnrecognizedMessage);
                return 0.0;
            }
            int literal = 0;
            while (position_ < text_.size() && is_digit(text_[position_])) {
                // Saturate rather than overflow; the range check below refuses
                // it either way.
                if (literal < 10'000) {
                    literal = literal * 10 + (text_[position_] - '0');
                }
                ++position_;
            }
            exponent += exponent_negative ? -literal : literal;
        }

        if (mantissa == 0U) {
            return negative ? -0.0 : 0.0;
        }
        if (exponent > max_decimal_exponent || exponent < -max_decimal_exponent) {
            fail(TrackerParseError::InvalidValue);
            return 0.0;
        }

        const double scale =
            exact_powers_of_ten[static_cast<std::size_t>(
                exponent < 0 ? -exponent : exponent)];
        const double digits = static_cast<double>(mantissa);
        const double value = exponent < 0 ? digits / scale : digits * scale;
        return negative ? -value : value;
    }

    // Consumes any JSON value, validating it completely.
    void skip_value(std::size_t depth) noexcept {
        if (error_.has_value()) {
            return;
        }
        if (depth > max_opentrackio_depth) {
            fail(TrackerParseError::InvalidValue);
            return;
        }
        const char c = peek();
        switch (c) {
        case '{':
            skip_object(depth);
            return;
        case '[':
            skip_array(depth);
            return;
        case '"':
            (void)read_string();
            return;
        case 't':
            expect_literal("true");
            return;
        case 'f':
            expect_literal("false");
            return;
        case 'n':
            expect_literal("null");
            return;
        case '\0':
            fail(TrackerParseError::Truncated);
            return;
        default:
            (void)read_number();
            return;
        }
    }

    // Opens an object and reports whether another member follows. The caller
    // reads the key with read_string() and then the value.
    [[nodiscard]] bool begin_object() noexcept { return consume('{'); }

    // True when a member key follows; false at the closing brace or on error.
    [[nodiscard]] bool next_member(bool& first) noexcept {
        if (error_.has_value()) {
            return false;
        }
        const char c = peek();
        if (c == '}') {
            ++position_;
            return false;
        }
        if (!first) {
            if (c != ',') {
                fail(c == '\0' ? TrackerParseError::Truncated
                               : TrackerParseError::UnrecognizedMessage);
                return false;
            }
            ++position_;
            if (peek() != '"') {
                fail(TrackerParseError::UnrecognizedMessage);
                return false;
            }
        } else if (c != '"') {
            fail(c == '\0' ? TrackerParseError::Truncated
                           : TrackerParseError::UnrecognizedMessage);
            return false;
        }
        first = false;
        return true;
    }

    [[nodiscard]] bool member_separator() noexcept { return consume(':'); }

    [[nodiscard]] bool begin_array() noexcept { return consume('['); }

    [[nodiscard]] bool next_element(bool& first) noexcept {
        if (error_.has_value()) {
            return false;
        }
        const char c = peek();
        if (c == ']') {
            ++position_;
            return false;
        }
        if (!first) {
            if (c != ',') {
                fail(c == '\0' ? TrackerParseError::Truncated
                               : TrackerParseError::UnrecognizedMessage);
                return false;
            }
            ++position_;
        } else if (c == '\0') {
            fail(TrackerParseError::Truncated);
            return false;
        }
        first = false;
        return true;
    }

private:
    [[nodiscard]] static bool is_digit(char c) noexcept {
        return c >= '0' && c <= '9';
    }

    [[nodiscard]] static bool is_hex(char c) noexcept {
        return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    // One digit into the running mantissa. `exponent_step` is 1 for a digit
    // before the decimal point and 0 for one after it, so digits past the
    // capacity of the mantissa still move the decimal exponent correctly.
    static void accumulate(char digit, core::u64& mantissa,
                           int& significant_digits, int& exponent,
                           int exponent_step) noexcept {
        const core::u64 value = static_cast<core::u64>(digit - '0');
        // Nineteen digits is the most that always fits a u64. The conversion
        // of the accumulated integer to binary64 is itself correctly rounded,
        // so this stays deterministic for longer literals as well.
        if (significant_digits < 19 && (mantissa != 0U || value != 0U)) {
            mantissa = mantissa * 10U + value;
            ++significant_digits;
            exponent -= 1 - exponent_step;
        } else if (significant_digits >= 19) {
            exponent += exponent_step;
        } else {
            // A leading zero in the fraction: no significant digit yet, but the
            // decimal point still moves.
            exponent -= 1 - exponent_step;
        }
    }

    void expect_literal(std::string_view literal) noexcept {
        skip_whitespace();
        if (text_.size() - position_ < literal.size()
            || text_.substr(position_, literal.size()) != literal) {
            fail(text_.size() - position_ < literal.size()
                     ? TrackerParseError::Truncated
                     : TrackerParseError::UnrecognizedMessage);
            return;
        }
        position_ += literal.size();
    }

    void skip_object(std::size_t depth) noexcept {
        if (!begin_object()) {
            return;
        }
        bool first = true;
        while (next_member(first)) {
            (void)read_string();
            if (!member_separator()) {
                return;
            }
            skip_value(depth + 1U);
            if (error_.has_value()) {
                return;
            }
        }
    }

    void skip_array(std::size_t depth) noexcept {
        if (!begin_array()) {
            return;
        }
        bool first = true;
        while (next_element(first)) {
            skip_value(depth + 1U);
            if (error_.has_value()) {
                return;
            }
        }
    }

    std::string_view text_;
    std::size_t position_{};
    std::optional<TrackerParseError> error_;
};

struct Parsed final {
    std::array<TransformEntry, max_opentrackio_transforms> transforms{};
    std::size_t transform_count{};
    bool transforms_overflowed{};
    double focal_length_mm{};
    double focus_distance_m{};
    double f_stop{};
    double entrance_pupil_offset_m{};
    core::u64 sequence{};
    bool has_sequence{};
    core::u32 version_major{};
    bool protocol_named{};
    // Which camera on the tracking system this sample describes. FreeD puts
    // this in every datagram; OpenTrackIO makes it optional, so absence has to
    // stay distinguishable from zero.
    core::u32 source_number{};
    bool has_source_number{};
};

void read_vector(Reader& reader, std::size_t depth, Vec3d& out,
                 bool& present) noexcept {
    if (!reader.begin_object()) {
        return;
    }
    bool first = true;
    bool saw_x = false;
    bool saw_y = false;
    bool saw_z = false;
    while (reader.next_member(first)) {
        const std::string_view key = reader.read_string();
        if (!reader.member_separator()) {
            return;
        }
        if (key == "x") {
            out.x = reader.read_number();
            saw_x = true;
        } else if (key == "y") {
            out.y = reader.read_number();
            saw_y = true;
        } else if (key == "z") {
            out.z = reader.read_number();
            saw_z = true;
        } else {
            reader.skip_value(depth + 1U);
        }
        if (reader.error().has_value()) {
            return;
        }
    }
    present = saw_x && saw_y && saw_z;
}

void read_rotation(Reader& reader, std::size_t depth,
                   TransformEntry& out) noexcept {
    if (!reader.begin_object()) {
        return;
    }
    bool first = true;
    bool saw_pan = false;
    bool saw_tilt = false;
    bool saw_roll = false;
    while (reader.next_member(first)) {
        const std::string_view key = reader.read_string();
        if (!reader.member_separator()) {
            return;
        }
        if (key == "pan") {
            out.pan = reader.read_number();
            saw_pan = true;
        } else if (key == "tilt") {
            out.tilt = reader.read_number();
            saw_tilt = true;
        } else if (key == "roll") {
            out.roll = reader.read_number();
            saw_roll = true;
        } else {
            reader.skip_value(depth + 1U);
        }
        if (reader.error().has_value()) {
            return;
        }
    }
    out.has_rotation = saw_pan && saw_tilt && saw_roll;
}

void read_transform(Reader& reader, std::size_t depth,
                    TransformEntry& out) noexcept {
    if (!reader.begin_object()) {
        return;
    }
    bool first = true;
    while (reader.next_member(first)) {
        const std::string_view key = reader.read_string();
        if (!reader.member_separator()) {
            return;
        }
        if (key == "translation") {
            read_vector(reader, depth + 1U, out.translation,
                        out.has_translation);
        } else if (key == "rotation") {
            read_rotation(reader, depth + 1U, out);
        } else if (key == "id") {
            out.is_camera = reader.read_string() == "Camera";
        } else {
            reader.skip_value(depth + 1U);
        }
        if (reader.error().has_value()) {
            return;
        }
    }
}

void read_transforms(Reader& reader, std::size_t depth, Parsed& out) noexcept {
    if (!reader.begin_array()) {
        return;
    }
    bool first = true;
    while (reader.next_element(first)) {
        TransformEntry entry{};
        read_transform(reader, depth + 1U, entry);
        if (reader.error().has_value()) {
            return;
        }
        if (out.transform_count < out.transforms.size()) {
            out.transforms[out.transform_count] = entry;
            ++out.transform_count;
        } else {
            // A chain longer than the bound is refused rather than silently
            // truncated: the entry that was dropped could have been the camera.
            out.transforms_overflowed = true;
        }
    }
}

void read_lens(Reader& reader, std::size_t depth, Parsed& out) noexcept {
    if (!reader.begin_object()) {
        return;
    }
    bool first = true;
    while (reader.next_member(first)) {
        const std::string_view key = reader.read_string();
        if (!reader.member_separator()) {
            return;
        }
        if (key == "focalLength") {
            out.focal_length_mm = reader.read_number();
        } else if (key == "focusDistance") {
            out.focus_distance_m = reader.read_number();
        } else if (key == "fStop") {
            out.f_stop = reader.read_number();
        } else if (key == "entrancePupilOffset") {
            out.entrance_pupil_offset_m = reader.read_number();
        } else {
            reader.skip_value(depth + 1U);
        }
        if (reader.error().has_value()) {
            return;
        }
    }
}

void read_timing(Reader& reader, std::size_t depth, Parsed& out) noexcept {
    if (!reader.begin_object()) {
        return;
    }
    bool first = true;
    while (reader.next_member(first)) {
        const std::string_view key = reader.read_string();
        if (!reader.member_separator()) {
            return;
        }
        if (key == "sequenceNumber") {
            const double value = reader.read_number();
            if (!(value >= 0.0) || value > 18'446'744'073'709'549'568.0) {
                reader.fail(TrackerParseError::InvalidValue);
                return;
            }
            out.sequence = static_cast<core::u64>(value);
            out.has_sequence = true;
        } else {
            reader.skip_value(depth + 1U);
        }
        if (reader.error().has_value()) {
            return;
        }
    }
}

void read_protocol(Reader& reader, std::size_t depth, Parsed& out) noexcept {
    if (!reader.begin_object()) {
        return;
    }
    bool first = true;
    while (reader.next_member(first)) {
        const std::string_view key = reader.read_string();
        if (!reader.member_separator()) {
            return;
        }
        if (key == "name") {
            out.protocol_named = reader.read_string() == "OpenTrackIO";
        } else if (key == "version") {
            if (!reader.begin_array()) {
                return;
            }
            bool element_first = true;
            std::size_t index = 0U;
            while (reader.next_element(element_first)) {
                const double value = reader.read_number();
                if (index == 0U && value >= 0.0 && value < 65'536.0) {
                    out.version_major = static_cast<core::u32>(value);
                }
                ++index;
                if (reader.error().has_value()) {
                    return;
                }
            }
        } else {
            reader.skip_value(depth + 1U);
        }
        if (reader.error().has_value()) {
            return;
        }
    }
}

[[nodiscard]] Turns turns_from_degrees(double degrees) noexcept {
    return static_cast<Turns>(degrees / 360.0);
}

[[nodiscard]] bool finite_and_bounded(double value, double limit) noexcept {
    return std::isfinite(value) && value >= -limit && value <= limit;
}

} // namespace

Result parse_opentrackio(
    std::span<const std::byte> datagram, core::u64 fallback_sequence) noexcept {
    if (datagram.empty()) {
        return Result::failure(TrackerParseError::Truncated);
    }
    if (datagram.size() > max_opentrackio_sample_bytes) {
        return Result::failure(TrackerParseError::UnrecognizedMessage);
    }

    const std::string_view text{
        reinterpret_cast<const char*>(datagram.data()), datagram.size()};
    Reader reader{text};
    if (!reader.begin_object()) {
        return Result::failure(
            reader.error().value_or(TrackerParseError::UnrecognizedMessage));
    }

    Parsed parsed{};
    bool first = true;
    while (reader.next_member(first)) {
        const std::string_view key = reader.read_string();
        if (!reader.member_separator()) {
            break;
        }
        if (key == "transforms") {
            read_transforms(reader, 1U, parsed);
        } else if (key == "lens") {
            read_lens(reader, 1U, parsed);
        } else if (key == "timing") {
            read_timing(reader, 1U, parsed);
        } else if (key == "protocol") {
            read_protocol(reader, 1U, parsed);
        } else if (key == "sourceNumber") {
            // The analogue of FreeD's camera id: which camera on the tracking
            // system this sample is for. Without it a feed authored for any
            // camera but zero refused every datagram it received, because the
            // listener filters on an id the parser never set.
            const double value = reader.read_number();
            if (!(value >= 0.0) || value > 255.0) {
                reader.fail(TrackerParseError::InvalidValue);
                break;
            }
            parsed.source_number = static_cast<core::u32>(value);
            parsed.has_source_number = true;
        } else {
            reader.skip_value(1U);
        }
        if (reader.error().has_value()) {
            break;
        }
    }
    if (reader.error().has_value()) {
        return Result::failure(*reader.error());
    }
    // Trailing bytes after the closing brace mean two coalesced datagrams or a
    // corrupted one. Either way this is not one sample.
    if (!reader.at_end()) {
        return Result::failure(TrackerParseError::UnrecognizedMessage);
    }

    if (!parsed.protocol_named) {
        return Result::failure(TrackerParseError::UnrecognizedMessage);
    }
    if (parsed.transforms_overflowed) {
        return Result::failure(TrackerParseError::InvalidValue);
    }

    const TransformEntry* selected = nullptr;
    bool named_camera = false;
    for (std::size_t index = 0U; index < parsed.transform_count; ++index) {
        const TransformEntry& entry = parsed.transforms[index];
        if (entry.is_camera) {
            selected = &entry;
            named_camera = true;
            break;
        }
    }
    if (selected == nullptr && parsed.transform_count != 0U) {
        selected = &parsed.transforms[parsed.transform_count - 1U];
    }
    if (selected == nullptr || !selected->has_translation
        || !selected->has_rotation) {
        return Result::failure(TrackerParseError::UnrecognizedMessage);
    }

    // A stage is metres across and a rotation is degrees. Values far outside
    // those are a unit mistake or a corrupt sample, and holding the previous
    // pose is better than swinging the wall.
    constexpr double position_limit = 100'000.0;
    constexpr double angle_limit = 100'000.0;
    constexpr double lens_limit = 100'000.0;
    if (!finite_and_bounded(selected->translation.x, position_limit)
        || !finite_and_bounded(selected->translation.y, position_limit)
        || !finite_and_bounded(selected->translation.z, position_limit)
        || !finite_and_bounded(selected->pan, angle_limit)
        || !finite_and_bounded(selected->tilt, angle_limit)
        || !finite_and_bounded(selected->roll, angle_limit)) {
        return Result::failure(TrackerParseError::InvalidValue);
    }
    if (parsed.focal_length_mm < 0.0 || parsed.focus_distance_m < 0.0
        || parsed.f_stop < 0.0
        || !finite_and_bounded(parsed.focal_length_mm, lens_limit)
        || !finite_and_bounded(parsed.focus_distance_m, lens_limit)
        || !finite_and_bounded(parsed.f_stop, lens_limit)
        || !finite_and_bounded(parsed.entrance_pupil_offset_m, lens_limit)) {
        return Result::failure(TrackerParseError::InvalidValue);
    }

    OpenTrackIOSample sample{};
    sample.pose.protocol = TrackerProtocol::OpenTrackIO;
    sample.pose.camera_id = parsed.source_number;
    sample.identifies_camera = parsed.has_source_number;
    sample.pose.x = static_cast<float>(selected->translation.x);
    sample.pose.y = static_cast<float>(selected->translation.y);
    sample.pose.z = static_cast<float>(selected->translation.z);
    sample.pose.pan = turns_from_degrees(selected->pan);
    sample.pose.tilt = turns_from_degrees(selected->tilt);
    sample.pose.roll = turns_from_degrees(selected->roll);
    sample.pose.focal_length_mm = static_cast<float>(parsed.focal_length_mm);
    sample.pose.focus_distance_m = static_cast<float>(parsed.focus_distance_m);
    sample.pose.aperture_f_number = static_cast<float>(parsed.f_stop);
    // OpenTrackIO names calibrated physical lens controls rather than raw
    // hardware counts. They are still encoder coordinates as long as the
    // profile was authored in the same units, and keeping that distinction in
    // the source/profile avoids inventing a FreeD-style mapping here.
    sample.pose.zoom_encoder = sample.pose.focal_length_mm;
    sample.pose.focus_encoder = sample.pose.focus_distance_m;
    sample.pose.iris_encoder = sample.pose.aperture_f_number;
    sample.pose.zoom_encoder_valid = sample.pose.zoom_encoder > 0.0F;
    sample.pose.focus_encoder_valid = sample.pose.focus_encoder > 0.0F;
    sample.pose.iris_encoder_valid = sample.pose.iris_encoder > 0.0F;
    sample.pose.sequence =
        parsed.has_sequence ? parsed.sequence : fallback_sequence;
    sample.entrance_pupil_offset_m =
        static_cast<float>(parsed.entrance_pupil_offset_m);
    sample.version_major = parsed.version_major;
    sample.named_camera_transform = named_camera;

    if (!valid(sample.pose)) {
        return Result::failure(TrackerParseError::InvalidValue);
    }
    return Result::success(sample);
}

} // namespace mediaserver::track
